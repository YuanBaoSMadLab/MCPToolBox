// Copyright MCPToolbox. All Rights Reserved.

#include "MCPToolboxMCPClient.h"
#include "MCPToolbox.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Common/TcpSocketBuilder.h"
#include "Async/Async.h"

// NOTE: LogMCPToolbox is defined in MCPToolboxAPIManager.cpp
// Do not duplicate DEFINE_LOG_CATEGORY here.

// Static member initialization
int32 FMCPToolboxMCPClient::MessageIdCounter = 0;
FThreadSafeCounter FMCPToolboxMCPClient::StaticMessageIdCounter(0);

// ============================================================================
// FMCPToolboxMCPMessage Implementation
// ============================================================================

FString FMCPToolboxMCPMessage::ToJsonString() const
{
	TSharedPtr<FJsonObject> JsonObj = MakeShareable(new FJsonObject());
	JsonObj->SetStringField(TEXT("jsonrpc"), JsonRPC);

	if (Id.IsValid())
	{
		JsonObj->SetField(TEXT("id"), Id);
	}

	if (!Method.IsEmpty())
	{
		JsonObj->SetStringField(TEXT("method"), Method);
	}

	if (Params.IsValid())
	{
		JsonObj->SetObjectField(TEXT("params"), Params);
	}

	if (Result.IsValid())
	{
		JsonObj->SetObjectField(TEXT("result"), Result);
	}

	if (Error.IsValid())
	{
		JsonObj->SetObjectField(TEXT("error"), Error);
	}

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(JsonObj.ToSharedRef(), Writer);
	return OutputString;
}

bool FMCPToolboxMCPMessage::FromJsonString(const FString& JsonString, FMCPToolboxMCPMessage& OutMessage)
{
	TSharedPtr<FJsonObject> JsonObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("FMCPToolboxMCPMessage::FromJsonString - Failed to parse JSON: %s"), *JsonString);
		return false;
	}

	return FromJsonObject(JsonObj, OutMessage);
}

bool FMCPToolboxMCPMessage::FromJsonObject(const TSharedPtr<FJsonObject>& JsonObj, FMCPToolboxMCPMessage& OutMessage)
{
	if (!JsonObj.IsValid())
	{
		return false;
	}

	OutMessage = FMCPToolboxMCPMessage();

	// jsonrpc
	JsonObj->TryGetStringField(TEXT("jsonrpc"), OutMessage.JsonRPC);

	// id (could be number, string, or null)
	OutMessage.Id = JsonObj->TryGetField(TEXT("id"));

	// method
	JsonObj->TryGetStringField(TEXT("method"), OutMessage.Method);

	// params
	const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("params"), ParamsObj))
	{
		OutMessage.Params = *ParamsObj;
	}

	// result
	const TSharedPtr<FJsonObject>* ResultObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("result"), ResultObj))
	{
		OutMessage.Result = *ResultObj;
	}

	// error
	const TSharedPtr<FJsonObject>* ErrorObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("error"), ErrorObj))
	{
		OutMessage.Error = *ErrorObj;
	}

	return true;
}

bool FMCPToolboxMCPMessage::IsNotification() const
{
	return !Id.IsValid();
}

bool FMCPToolboxMCPMessage::IsRequest() const
{
	return Id.IsValid() && !Method.IsEmpty();
}

bool FMCPToolboxMCPMessage::IsResponse() const
{
	return Id.IsValid() && Method.IsEmpty();
}

bool FMCPToolboxMCPMessage::IsError() const
{
	return Error.IsValid();
}

// ============================================================================
// FMCPToolboxMCPTool Implementation
// ============================================================================

FMCPToolboxMCPTool FMCPToolboxMCPTool::FromJson(const TSharedPtr<FJsonObject>& JsonObj)
{
	FMCPToolboxMCPTool Tool;

	if (!JsonObj.IsValid())
	{
		return Tool;
	}

	JsonObj->TryGetStringField(TEXT("name"), Tool.Name);
	JsonObj->TryGetStringField(TEXT("description"), Tool.Description);

	const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
	if (JsonObj->TryGetObjectField(TEXT("inputSchema"), SchemaObj))
	{
		Tool.InputSchema = *SchemaObj;
	}

	return Tool;
}

// ============================================================================
// FMCPToolboxMCPResource Implementation
// ============================================================================

FMCPToolboxMCPResource FMCPToolboxMCPResource::FromJson(const TSharedPtr<FJsonObject>& JsonObj)
{
	FMCPToolboxMCPResource Resource;

	if (!JsonObj.IsValid())
	{
		return Resource;
	}

	JsonObj->TryGetStringField(TEXT("uri"), Resource.Uri);
	JsonObj->TryGetStringField(TEXT("name"), Resource.Name);
	JsonObj->TryGetStringField(TEXT("mimeType"), Resource.MimeType);
	JsonObj->TryGetStringField(TEXT("description"), Resource.Description);

	return Resource;
}

// ============================================================================
// FMCPToolboxMCPClient Implementation
// ============================================================================

FMCPToolboxMCPClient::FMCPToolboxMCPClient()
	: Socket(nullptr)
	, WorkerThread(nullptr)
	, bShouldRun(false)
	, bIsShuttingDown(false)
	, StateCounter(0)
	, bIsConnected(false)
	, ServerHost(TEXT("127.0.0.1"))
	, ServerPort(8000)
	, ConnectionTimeoutSeconds(10)
	, RequestTimeoutSeconds(60)
	, bAutoReconnect(true)
	, MaxReconnectAttempts(5)
	, HeartbeatIntervalSeconds(15)
	, LastHeartbeatTime(0.0)
	, ReconnectAttempts(0)
	, bEncryptionEnabled(false)
{
	StaticMessageIdCounter.Set(0);
}

FMCPToolboxMCPClient::~FMCPToolboxMCPClient()
{
	Disconnect();
}

// ============================================================================
// Connection Management
// ============================================================================

bool FMCPToolboxMCPClient::Connect(const FString& Host, int32 Port)
{
	if (bIsConnected)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("Already connected. Disconnect first before reconnecting."));
		return false;
	}

	ServerHost = Host;
	ServerPort = Port;

	UE_LOG(LogMCPToolbox, Log, TEXT("Connecting to MCP server at %s:%d..."), *ServerHost, ServerPort);

	StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Connecting));

	// Resolve address
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("Failed to get socket subsystem."));
		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
		OnError.Broadcast(-1, TEXT("Failed to get socket subsystem."));
		return false;
	}

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	Addr->SetIp(*ServerHost, bIsValid);
	if (!bIsValid)
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("Invalid host address: %s"), *ServerHost);
		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
		OnError.Broadcast(-2, FString::Printf(TEXT("Invalid host address: %s"), *ServerHost));
		return false;
	}
	Addr->SetPort(ServerPort);

	// Create TCP socket
	Socket = FTcpSocketBuilder(TEXT("MCPToolboxClient"))
		.AsReusable()
		.WithReceiveBufferSize(65536)
		.WithSendBufferSize(65536);

	if (!Socket)
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("Failed to create TCP socket."));
		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
		OnError.Broadcast(-3, TEXT("Failed to create TCP socket."));
		return false;
	}

	// Set non-blocking mode with timeout
	Socket->SetNonBlocking(true);

	// Attempt connection
	if (!Socket->Connect(*Addr))
	{
		ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
		if (Error != SE_EWOULDBLOCK && Error != SE_EINPROGRESS)
		{
			UE_LOG(LogMCPToolbox, Error, TEXT("Failed to connect to %s:%d - Error: %d"),
				*ServerHost, ServerPort, static_cast<int32>(Error));
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;

			StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
			OnError.Broadcast(static_cast<int32>(Error),
				FString::Printf(TEXT("Failed to connect to %s:%d"), *ServerHost, ServerPort));
			return false;
		}

		// Wait for connection to complete (non-blocking in progress)
		UE_LOG(LogMCPToolbox, Log, TEXT("Connection pending, waiting up to %d seconds..."), ConnectionTimeoutSeconds);

		double StartTime = FPlatformTime::Seconds();
		ESocketConnectionState ConnectionState = SCS_ConnectionError;

		while (FPlatformTime::Seconds() - StartTime < ConnectionTimeoutSeconds)
		{
			ConnectionState = Socket->GetConnectionState();
			if (ConnectionState == SCS_Connected)
			{
				break;
			}
			FPlatformProcess::Sleep(0.1f);
		}

		if (ConnectionState != SCS_Connected)
		{
			UE_LOG(LogMCPToolbox, Error, TEXT("Connection timed out after %d seconds."), ConnectionTimeoutSeconds);
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;

			StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
			OnError.Broadcast(-4, TEXT("Connection timed out."));
			return false;
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("TCP socket connected to %s:%d successfully."), *ServerHost, ServerPort);

	bIsConnected = true;
	bShouldRun = true;
	bIsShuttingDown = false;
	ReconnectAttempts = 0;
	LastHeartbeatTime = FPlatformTime::Seconds();

	// Start the worker thread
	WorkerThread = FRunnableThread::Create(this, TEXT("MCPToolboxClientWorker"), 0, TPri_Normal);
	if (!WorkerThread)
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("Failed to create worker thread."));
		bShouldRun = false;
		bIsConnected = false;
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;

		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
		OnError.Broadcast(-5, TEXT("Failed to create worker thread."));
		return false;
	}

	StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Connected));
	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client connected and worker thread started."));

	// Broadcast connected event on the game thread
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnConnected.Broadcast();
	});

	return true;
}

void FMCPToolboxMCPClient::Disconnect()
{
	if (bIsShuttingDown)
	{
		return;
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("Disconnecting MCP client from %s:%d..."), *ServerHost, ServerPort);

	bIsShuttingDown = true;
	bShouldRun = false;

	// Wait for worker thread to finish
	if (WorkerThread)
	{
		WorkerThread->Kill(true);
		delete WorkerThread;
		WorkerThread = nullptr;
	}

	// Close and destroy socket
	{
		FScopeLock Lock(&SocketCriticalSection);
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	// Clear pending promises
	{
		FScopeLock Lock(&PendingResponsesCriticalSection);
		for (auto& Pair : PendingPromises)
		{
			if (Pair.Value.IsValid())
			{
				Pair.Value->SetValue(nullptr);
			}
		}
		PendingPromises.Empty();
		PendingResponses.Empty();
	}

	bIsConnected = false;
	StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Disconnected));

	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client disconnected."));

	// Broadcast disconnected event on the game thread
	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnDisconnected.Broadcast();
	});
}

bool FMCPToolboxMCPClient::IsConnected() const
{
	return bIsConnected;
}

EMCPToolboxMCPClientState FMCPToolboxMCPClient::GetState() const
{
	return static_cast<EMCPToolboxMCPClientState>(StateCounter.GetValue());
}

// ============================================================================
// Settings Accessors
// ============================================================================

void FMCPToolboxMCPClient::SetConnectionTimeout(int32 TimeoutSeconds)
{
	ConnectionTimeoutSeconds = FMath::Clamp(TimeoutSeconds, 1, 120);
}

void FMCPToolboxMCPClient::SetRequestTimeout(int32 TimeoutSeconds)
{
	RequestTimeoutSeconds = FMath::Clamp(TimeoutSeconds, 1, 300);
}

void FMCPToolboxMCPClient::SetAutoReconnect(bool bInAutoReconnect)
{
	bAutoReconnect = bInAutoReconnect;
}

void FMCPToolboxMCPClient::SetMaxReconnectAttempts(int32 MaxAttempts)
{
	MaxReconnectAttempts = FMath::Clamp(MaxAttempts, 1, 20);
}

void FMCPToolboxMCPClient::SetHeartbeatInterval(int32 IntervalSeconds)
{
	HeartbeatIntervalSeconds = FMath::Clamp(IntervalSeconds, 5, 120);
}

void FMCPToolboxMCPClient::SetEncryptionEnabled(bool bEnabled)
{
	bEncryptionEnabled = bEnabled;
	UE_LOG(LogMCPToolbox, Log, TEXT("Encryption %s"), bEnabled ? TEXT("enabled") : TEXT("disabled"));
}

void FMCPToolboxMCPClient::SetEncryptionKey(const FString& Key)
{
	EncryptionKey = Key;
	if (!Key.IsEmpty())
	{
		bEncryptionEnabled = true;
		UE_LOG(LogMCPToolbox, Log, TEXT("Encryption key set, encryption enabled."));
	}
}

// ============================================================================
// FRunnable Interface
// ============================================================================

bool FMCPToolboxMCPClient::Init()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client worker thread initializing..."));
	return true;
}

uint32 FMCPToolboxMCPClient::Run()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client worker thread started."));

	const int32 BufferSize = 65536;
	TArray<uint8> ReceiveBuffer;
	ReceiveBuffer.SetNumZeroed(BufferSize);

	FString AccumulatedData;

	while (bShouldRun && !bIsShuttingDown)
	{
		if (!Socket)
		{
			FPlatformProcess::Sleep(0.1f);
			continue;
		}

		// Check for incoming data
		{
			FScopeLock Lock(&SocketCriticalSection);

			int32 BytesRead = 0;
			if (Socket->Recv(ReceiveBuffer.GetData(), BufferSize, BytesRead, ESocketReceiveFlags::None))
			{
				if (BytesRead > 0)
				{
					// Append received data
					FString ReceivedData = FString(BytesRead, UTF8_TO_TCHAR(reinterpret_cast<const char*>(ReceiveBuffer.GetData())));

					// Decrypt if needed
					if (bEncryptionEnabled)
					{
						ReceivedData = DecryptData(ReceivedData);
					}

					AccumulatedData += ReceivedData;

					// Process complete messages separated by newline
					int32 NewlinePos;
					while ((NewlinePos = AccumulatedData.Find(TEXT("\n"))) != INDEX_NONE)
					{
						FString MessageStr = AccumulatedData.Left(NewlinePos);
						AccumulatedData.RemoveAt(0, NewlinePos + 1);

						MessageStr.TrimStartAndEndInline();
						if (!MessageStr.IsEmpty())
						{
							TSharedPtr<FJsonObject> MessageObj;
							if (ParseMessageFromBuffer(MessageStr, MessageObj))
							{
								// Process on game thread
								TSharedPtr<FJsonObject> MessageCopy = MessageObj;
								AsyncTask(ENamedThreads::GameThread, [this, MessageCopy]()
								{
									ProcessIncomingMessage(MessageCopy);
								});
							}
						}
					}
				}
				else if (BytesRead == 0)
				{
					// Connection closed by server
					UE_LOG(LogMCPToolbox, Warning, TEXT("Connection closed by server (read 0 bytes)."));
					bIsConnected = false;
					StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Disconnected));

					AsyncTask(ENamedThreads::GameThread, [this]()
					{
						OnDisconnected.Broadcast();
					});

					// Attempt auto-reconnect
					if (bAutoReconnect && !bIsShuttingDown)
					{
						PerformReconnect();
					}
					break;
				}
			}
			else
			{
				ESocketErrors Error = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
				if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
				{
					UE_LOG(LogMCPToolbox, Warning, TEXT("Socket receive error: %d"), static_cast<int32>(Error));

					StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));

					AsyncTask(ENamedThreads::GameThread, [this, Error]()
					{
						OnError.Broadcast(static_cast<int32>(Error), TEXT("Socket receive error."));
					});

					// Attempt auto-reconnect
					if (bAutoReconnect && !bIsShuttingDown)
					{
						PerformReconnect();
					}
					else
					{
						bShouldRun = false;
						bIsConnected = false;
					}
					break;
				}
			}
		}

		// Heartbeat logic
		double CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LastHeartbeatTime >= HeartbeatIntervalSeconds)
		{
			SendHeartbeat();
			LastHeartbeatTime = CurrentTime;
		}

		// Sleep to prevent busy-waiting
		FPlatformProcess::Sleep(0.01f);
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client worker thread exiting."));
	return 0;
}

void FMCPToolboxMCPClient::Stop()
{
	bShouldRun = false;
	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client Stop() called."));
}

void FMCPToolboxMCPClient::Exit()
{
	UE_LOG(LogMCPToolbox, Log, TEXT("MCP client Exit() called."));
}

// ============================================================================
// Internal Helpers
// ============================================================================

int32 FMCPToolboxMCPClient::GenerateMessageId()
{
	const int32 Id = StaticMessageIdCounter.Increment();
	return Id;
}

FString FMCPToolboxMCPClient::SerializeRequest(const FString& Method, int32 Id, const TSharedPtr<FJsonObject>& Params, bool bIsNotification)
{
	TSharedPtr<FJsonObject> RequestObj = MakeShareable(new FJsonObject());

	RequestObj->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	RequestObj->SetStringField(TEXT("method"), Method);

	if (bIsNotification)
	{
		// Notifications have no id
	}
	else
	{
		RequestObj->SetNumberField(TEXT("id"), static_cast<double>(Id));
	}

	if (Params.IsValid())
	{
		RequestObj->SetObjectField(TEXT("params"), Params);
	}
	else
	{
		RequestObj->SetObjectField(TEXT("params"), MakeShareable(new FJsonObject()));
	}

	FString OutputString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&OutputString);
	FJsonSerializer::Serialize(RequestObj.ToSharedRef(), Writer);

	return OutputString;
}

bool FMCPToolboxMCPClient::ParseMessageFromBuffer(const FString& Buffer, TSharedPtr<FJsonObject>& OutMessage)
{
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Buffer);

	if (!FJsonSerializer::Deserialize(Reader, OutMessage) || !OutMessage.IsValid())
	{
		UE_LOG(LogMCPToolbox, Verbose, TEXT("Failed to parse message from buffer: %s"), *Buffer.Left(200));
		return false;
	}

	return true;
}

bool FMCPToolboxMCPClient::SendRawData(const FString& Data)
{
	FScopeLock Lock(&SocketCriticalSection);

	if (!Socket)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("SendRawData: Socket is null."));
		return false;
	}

	FString DataToSend = Data;
	if (bEncryptionEnabled)
	{
		DataToSend = EncryptData(Data);
	}

	// Ensure the data ends with a newline delimiter
	if (!DataToSend.EndsWith(TEXT("\n")))
	{
		DataToSend += TEXT("\n");
	}

	FTCHARToUTF8 Converter(*DataToSend);
	const char* RawData = Converter.Get();
	int32 DataSize = Converter.Length();

	int32 BytesSent = 0;
	bool bSuccess = Socket->Send(reinterpret_cast<const uint8*>(RawData), DataSize, BytesSent);

	if (!bSuccess || BytesSent != DataSize)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("SendRawData: Failed to send all data. Sent %d of %d bytes."), BytesSent, DataSize);
		return false;
	}

	return true;
}

FString FMCPToolboxMCPClient::EncryptData(const FString& PlainText) const
{
	// Simple XOR cipher with the encryption key
	if (EncryptionKey.IsEmpty())
	{
		return PlainText;
	}

	FString Combined = EncryptionKey + TEXT(":") + PlainText;
	return ApplyCipher(Combined);
}

FString FMCPToolboxMCPClient::DecryptData(const FString& CipherText) const
{
	if (EncryptionKey.IsEmpty())
	{
		return CipherText;
	}

	FString Decrypted = ApplyCipher(CipherText);

	// Strip the key prefix
	int32 SeparatorPos = Decrypted.Find(TEXT(":"));
	if (SeparatorPos != INDEX_NONE)
	{
		FString Prefix = Decrypted.Left(SeparatorPos);
		if (Prefix == EncryptionKey)
		{
			return Decrypted.Mid(SeparatorPos + 1);
		}
	}

	return Decrypted;
}

FString FMCPToolboxMCPClient::ApplyCipher(const FString& Input) const
{
	// Simple XOR obfuscation using the encryption key as a keystream
	if (EncryptionKey.IsEmpty() || Input.IsEmpty())
	{
		return Input;
	}

	FString Result;
	Result.Reserve(Input.Len());

	// Convert key to a byte-like key for XOR
	TArray<int32> KeyBytes;
	for (int32 i = 0; i < EncryptionKey.Len(); ++i)
	{
		KeyBytes.Add(static_cast<int32>(EncryptionKey[i]));
	}

	for (int32 i = 0; i < Input.Len(); ++i)
	{
		int32 KeyByte = KeyBytes[i % KeyBytes.Num()];
		TCHAR EncodedChar = static_cast<TCHAR>(static_cast<int32>(Input[i]) ^ KeyByte);
		Result += EncodedChar;
	}

	return Result;
}

bool FMCPToolboxMCPClient::PerformReconnect()
{
	if (!bAutoReconnect || bIsShuttingDown)
	{
		return false;
	}

	StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Reconnecting));

	// Close old socket
	{
		FScopeLock Lock(&SocketCriticalSection);
		if (Socket)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
	}

	while (ReconnectAttempts < MaxReconnectAttempts && bShouldRun && !bIsShuttingDown)
	{
		// Exponential backoff: 1s, 2s, 4s, 8s, 16s...
		float BackoffSeconds = FMath::Pow(2.0f, static_cast<float>(ReconnectAttempts));
		BackoffSeconds = FMath::Min(BackoffSeconds, 60.0f);  // Cap at 60 seconds

		UE_LOG(LogMCPToolbox, Log, TEXT("Reconnecting in %.0f seconds (attempt %d of %d)..."),
			BackoffSeconds, ReconnectAttempts + 1, MaxReconnectAttempts);

		// Wait with periodic checks for shutdown
		float Elapsed = 0.0f;
		while (Elapsed < BackoffSeconds && bShouldRun && !bIsShuttingDown)
		{
			FPlatformProcess::Sleep(0.1f);
			Elapsed += 0.1f;
		}

		if (!bShouldRun || bIsShuttingDown)
		{
			UE_LOG(LogMCPToolbox, Log, TEXT("Reconnect cancelled due to shutdown."));
			return false;
		}

		ReconnectAttempts++;

		// Resolve address
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			continue;
		}

		TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
		bool bIsValid = false;
		Addr->SetIp(*ServerHost, bIsValid);
		if (!bIsValid)
		{
			continue;
		}
		Addr->SetPort(ServerPort);

		// Create new socket
		FSocket* NewSocket = FTcpSocketBuilder(TEXT("MCPToolboxClient_Reconnect"))
			.AsReusable()
			.WithReceiveBufferSize(65536)
			.WithSendBufferSize(65536);

		if (!NewSocket)
		{
			continue;
		}

		NewSocket->SetNonBlocking(true);

		if (!NewSocket->Connect(*Addr))
		{
			ESocketErrors Error = SocketSubsystem->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_EINPROGRESS)
			{
				SocketSubsystem->DestroySocket(NewSocket);
				UE_LOG(LogMCPToolbox, Warning, TEXT("Reconnect attempt %d failed: error %d"),
					ReconnectAttempts, static_cast<int32>(Error));
				continue;
			}

			// Wait for connection
			double StartTime = FPlatformTime::Seconds();
			ESocketConnectionState ConnectionState = SCS_ConnectionError;

			while (FPlatformTime::Seconds() - StartTime < ConnectionTimeoutSeconds)
			{
				ConnectionState = NewSocket->GetConnectionState();
				if (ConnectionState == SCS_Connected)
				{
					break;
				}
				FPlatformProcess::Sleep(0.1f);
			}

			if (ConnectionState != SCS_Connected)
			{
				NewSocket->Close();
				SocketSubsystem->DestroySocket(NewSocket);
				UE_LOG(LogMCPToolbox, Warning, TEXT("Reconnect attempt %d timed out."), ReconnectAttempts);
				continue;
			}
		}

		// Success - assign the new socket
		{
			FScopeLock Lock(&SocketCriticalSection);
			Socket = NewSocket;
		}

		bIsConnected = true;
		ReconnectAttempts = 0;
		LastHeartbeatTime = FPlatformTime::Seconds();
		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Connected));

		UE_LOG(LogMCPToolbox, Log, TEXT("Reconnection successful."));

		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnConnected.Broadcast();
		});

		return true;
	}

	// Max retries exceeded
	StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));
	UE_LOG(LogMCPToolbox, Error, TEXT("Failed to reconnect after %d attempts. Giving up."), MaxReconnectAttempts);

	AsyncTask(ENamedThreads::GameThread, [this]()
	{
		OnError.Broadcast(-6, FString::Printf(TEXT("Failed to reconnect after %d attempts."), MaxReconnectAttempts));
	});

	return false;
}

void FMCPToolboxMCPClient::SendHeartbeat()
{
	if (!bIsConnected)
	{
		return;
	}

	// Use ping as heartbeat to verify connection is alive
	TSharedPtr<FJsonObject> EmptyParams = MakeShareable(new FJsonObject());
	TSharedPtr<FJsonObject> Response;
	if (!SendRequest(TEXT("ping"), EmptyParams, Response))
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("Heartbeat ping failed. Connection may be lost."));
		bIsConnected = false;

		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.Broadcast(-7, TEXT("Heartbeat failed - connection lost."));
		});

		StateCounter.Set(static_cast<int32>(EMCPToolboxMCPClientState::Error));

		if (bAutoReconnect && !bIsShuttingDown)
		{
			PerformReconnect();
		}
	}
	else
	{
		UE_LOG(LogMCPToolbox, Verbose, TEXT("Heartbeat OK."));
	}
}

void FMCPToolboxMCPClient::ProcessIncomingMessage(const TSharedPtr<FJsonObject>& Message)
{
	if (!Message.IsValid())
	{
		return;
	}

	FMCPToolboxMCPMessage MCPMessage;
	if (!FMCPToolboxMCPMessage::FromJsonObject(Message, MCPMessage))
	{
		return;
	}

	// Handle responses (match to pending requests)
	if (MCPMessage.IsResponse())
	{
		double IdValue = 0;
		int32 Id = 0;

		if (MCPMessage.Id.IsValid())
		{
			MCPMessage.Id->TryGetNumber(IdValue);
			Id = static_cast<int32>(IdValue);
		}

		if (Id > 0)
		{
			FScopeLock Lock(&PendingResponsesCriticalSection);
			TSharedPtr<TPromise<TSharedPtr<FJsonObject>>>* PromisePtr = PendingPromises.Find(Id);
			if (PromisePtr && PromisePtr->IsValid())
			{
				if (MCPMessage.IsError())
				{
					(*PromisePtr)->SetValue(nullptr);
					UE_LOG(LogMCPToolbox, Warning, TEXT("Error response received for request %d"), Id);
				}
				else
				{
					TSharedPtr<FJsonObject> ResultObj = MCPMessage.Result;
					if (ResultObj.IsValid())
					{
						(*PromisePtr)->SetValue(ResultObj);
					}
					else
					{
						// Some responses have result embedded in the same object
						(*PromisePtr)->SetValue(Message);
					}
				}
				PendingPromises.Remove(Id);
			}
		}
	}

	// Broadcast the raw message for external handling
	OnMessageReceived.Broadcast(Message);
}

// ============================================================================
// MCP Protocol Operations
// ============================================================================

bool FMCPToolboxMCPClient::SendRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResponse)
{
	if (!bIsConnected || !Socket)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("SendRequest '%s' failed: not connected."), *Method);
		return false;
	}

	int32 Id = GenerateMessageId();

	// Create a promise/future pair for the response
	TSharedPtr<TPromise<TSharedPtr<FJsonObject>>> Promise = MakeShareable(new TPromise<TSharedPtr<FJsonObject>>());
	TFuture<TSharedPtr<FJsonObject>> Future = Promise->GetFuture();

	{
		FScopeLock Lock(&PendingResponsesCriticalSection);
		PendingPromises.Add(Id, Promise);
	}

	// Serialize and send
	FString RequestStr = SerializeRequest(Method, Id, Params, false);
	if (!SendRawData(RequestStr))
	{
		FScopeLock Lock(&PendingResponsesCriticalSection);
		PendingPromises.Remove(Id);
		UE_LOG(LogMCPToolbox, Error, TEXT("SendRequest '%s' failed: could not send data."), *Method);
		return false;
	}

	UE_LOG(LogMCPToolbox, Verbose, TEXT("Sent request [%d] '%s'"), Id, *Method);

	// Wait for response with timeout
	double StartTime = FPlatformTime::Seconds();

	while (FPlatformTime::Seconds() - StartTime < RequestTimeoutSeconds)
	{
		if (Future.IsReady())
		{
			OutResponse = Future.Get();
			if (!OutResponse.IsValid())
			{
				UE_LOG(LogMCPToolbox, Warning, TEXT("Request [%d] '%s' returned error response."), Id, *Method);
				return false;
			}
			UE_LOG(LogMCPToolbox, Verbose, TEXT("Request [%d] '%s' completed successfully."), Id, *Method);
			return true;
		}

		if (!bIsConnected || bIsShuttingDown)
		{
			break;
		}

		FPlatformProcess::Sleep(0.01f);
	}

	// Timeout - clean up
	{
		FScopeLock Lock(&PendingResponsesCriticalSection);
		PendingPromises.Remove(Id);
	}

	UE_LOG(LogMCPToolbox, Warning, TEXT("Request [%d] '%s' timed out after %d seconds."), Id, *Method, RequestTimeoutSeconds);
	return false;
}

bool FMCPToolboxMCPClient::SendNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params)
{
	if (!bIsConnected || !Socket)
	{
		UE_LOG(LogMCPToolbox, Warning, TEXT("SendNotification '%s' failed: not connected."), *Method);
		return false;
	}

	FString NotificationStr = SerializeRequest(Method, 0, Params, true);
	bool bSuccess = SendRawData(NotificationStr);

	if (bSuccess)
	{
		UE_LOG(LogMCPToolbox, Verbose, TEXT("Sent notification '%s'"), *Method);
	}
	else
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("Failed to send notification '%s'"), *Method);
	}

	return bSuccess;
}

bool FMCPToolboxMCPClient::Initialize()
{
	TSharedPtr<FJsonObject> InitParams = MakeShareable(new FJsonObject());

	// Protocol version
	InitParams->SetStringField(TEXT("protocolVersion"), TEXT("2024-11-05"));

	// Client capabilities
	TSharedPtr<FJsonObject> Capabilities = MakeShareable(new FJsonObject());
	Capabilities->SetObjectField(TEXT("tools"), MakeShareable(new FJsonObject()));
	Capabilities->SetObjectField(TEXT("resources"), MakeShareable(new FJsonObject()));
	InitParams->SetObjectField(TEXT("capabilities"), Capabilities);

	// Client info
	TSharedPtr<FJsonObject> ClientInfo = MakeShareable(new FJsonObject());
	ClientInfo->SetStringField(TEXT("name"), TEXT("MCPToolbox"));
	ClientInfo->SetStringField(TEXT("version"), TEXT("1.0.0"));
	InitParams->SetObjectField(TEXT("clientInfo"), ClientInfo);

	TSharedPtr<FJsonObject> InitResult;
	if (!SendRequest(TEXT("initialize"), InitParams, InitResult))
	{
		UE_LOG(LogMCPToolbox, Error, TEXT("MCP initialization failed."));
		return false;
	}

	// Send initialized notification
	TSharedPtr<FJsonObject> EmptyParams = MakeShareable(new FJsonObject());
	SendNotification(TEXT("notifications/initialized"), EmptyParams);

	UE_LOG(LogMCPToolbox, Log, TEXT("MCP session initialized successfully."));
	return true;
}

bool FMCPToolboxMCPClient::Ping()
{
	TSharedPtr<FJsonObject> EmptyParams = MakeShareable(new FJsonObject());
	TSharedPtr<FJsonObject> Response;
	return SendRequest(TEXT("ping"), EmptyParams, Response);
}

bool FMCPToolboxMCPClient::CallTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments, TSharedPtr<FJsonObject>& OutResult)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
	Params->SetStringField(TEXT("name"), ToolName);
	if (Arguments.IsValid())
	{
		Params->SetObjectField(TEXT("arguments"), Arguments);
	}
	else
	{
		Params->SetObjectField(TEXT("arguments"), MakeShareable(new FJsonObject()));
	}

	return SendRequest(TEXT("tools/call"), Params, OutResult);
}

bool FMCPToolboxMCPClient::ListTools(TArray<FMCPToolboxMCPTool>& OutTools)
{
	TSharedPtr<FJsonObject> EmptyParams = MakeShareable(new FJsonObject());
	TSharedPtr<FJsonObject> Response;

	if (!SendRequest(TEXT("tools/list"), EmptyParams, Response))
	{
		return false;
	}

	if (!Response.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ToolsArray = nullptr;
	if (Response->TryGetArrayField(TEXT("tools"), ToolsArray))
	{
		for (const TSharedPtr<FJsonValue>& ToolValue : *ToolsArray)
		{
			const TSharedPtr<FJsonObject>* ToolObj = nullptr;
			if (ToolValue->TryGetObject(ToolObj))
			{
				FMCPToolboxMCPTool Tool = FMCPToolboxMCPTool::FromJson(*ToolObj);
				OutTools.Add(Tool);

				// Broadcast tool discovery
				OnToolDiscovered.Broadcast(*ToolObj);
			}
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("Discovered %d tools from MCP server."), OutTools.Num());
	return true;
}

bool FMCPToolboxMCPClient::ListResources(TArray<FMCPToolboxMCPResource>& OutResources)
{
	TSharedPtr<FJsonObject> EmptyParams = MakeShareable(new FJsonObject());
	TSharedPtr<FJsonObject> Response;

	if (!SendRequest(TEXT("resources/list"), EmptyParams, Response))
	{
		return false;
	}

	if (!Response.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* ResourcesArray = nullptr;
	if (Response->TryGetArrayField(TEXT("resources"), ResourcesArray))
	{
		for (const TSharedPtr<FJsonValue>& ResourceValue : *ResourcesArray)
		{
			const TSharedPtr<FJsonObject>* ResourceObj = nullptr;
			if (ResourceValue->TryGetObject(ResourceObj))
			{
				FMCPToolboxMCPResource Resource = FMCPToolboxMCPResource::FromJson(*ResourceObj);
				OutResources.Add(Resource);
			}
		}
	}

	UE_LOG(LogMCPToolbox, Log, TEXT("Discovered %d resources from MCP server."), OutResources.Num());
	return true;
}

bool FMCPToolboxMCPClient::ReadResource(const FString& ResourceUri, TSharedPtr<FJsonObject>& OutContent)
{
	TSharedPtr<FJsonObject> Params = MakeShareable(new FJsonObject());
	Params->SetStringField(TEXT("uri"), ResourceUri);

	return SendRequest(TEXT("resources/read"), Params, OutContent);
}
