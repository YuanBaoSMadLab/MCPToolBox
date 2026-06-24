// Copyright MCPToolbox. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "MCPToolboxMCPClient.generated.h"

// ============================================================================
// Forward Declarations
// ============================================================================

class FMCPToolboxMCPClient;

// ============================================================================
// Delegates
// ============================================================================

DECLARE_MULTICAST_DELEGATE(FOnMCPToolboxConnected);
DECLARE_MULTICAST_DELEGATE(FOnMCPDisconnected);
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMCPError, int32 /*ErrorCode*/, const FString& /*ErrorMessage*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPMessageReceived, const TSharedPtr<FJsonObject>& /*Message*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FOnMCPToolDiscovered, const TSharedPtr<FJsonObject>& /*ToolInfo*/);

// ============================================================================
// Enums
// ============================================================================

/** State of the MCP client connection. */
UENUM(BlueprintType)
enum class EMCPToolboxMCPClientState : uint8
{
	Disconnected	UMETA(DisplayName = "Disconnected"),
	Connecting		UMETA(DisplayName = "Connecting"),
	Connected		UMETA(DisplayName = "Connected"),
	Error			UMETA(DisplayName = "Error"),
	Reconnecting	UMETA(DisplayName = "Reconnecting"),
};

// ============================================================================
// Structs
// ============================================================================

/**
 * Represents a single JSON-RPC 2.0 message in the MCP protocol.
 */
struct MCPTOOLBOX_API FMCPToolboxMCPMessage
{
	/** JSON-RPC protocol version (always "2.0"). */
	FString JsonRPC = TEXT("2.0");

	/** Request/response ID. Can be a string, number, or null for notifications. */
	TSharedPtr<FJsonValue> Id;

	/** Method name for requests. */
	FString Method;

	/** Parameters for requests or result data for responses. */
	TSharedPtr<FJsonObject> Params;

	/** Result data (response only). */
	TSharedPtr<FJsonObject> Result;

	/** Error data (response only). */
	TSharedPtr<FJsonObject> Error;

	/** Serialize this message to a JSON string. */
	FString ToJsonString() const;

	/** Parse from a JSON string. Returns true on success. */
	static bool FromJsonString(const FString& JsonString, FMCPToolboxMCPMessage& OutMessage);

	/** Parse from an FJsonObject. Returns true on success. */
	static bool FromJsonObject(const TSharedPtr<FJsonObject>& JsonObj, FMCPToolboxMCPMessage& OutMessage);

	/** Check if this is a notification (no ID). */
	bool IsNotification() const;

	/** Check if this is a request (has method and ID). */
	bool IsRequest() const;

	/** Check if this is a response (has ID but no method). */
	bool IsResponse() const;

	/** Check if this is an error response. */
	bool IsError() const;
};

/**
 * Represents an MCP tool discovered from an MCP server.
 */
struct MCPTOOLBOX_API FMCPToolboxMCPTool
{
	/** Unique name of the tool. */
	FString Name;

	/** Human-readable description of the tool. */
	FString Description;

	/** JSON Schema for the tool's input parameters. */
	TSharedPtr<FJsonObject> InputSchema;

	/** Parse from a JSON object. */
	static FMCPToolboxMCPTool FromJson(const TSharedPtr<FJsonObject>& JsonObj);
};

/**
 * Represents an MCP resource discovered from an MCP server.
 */
struct MCPTOOLBOX_API FMCPToolboxMCPResource
{
	/** URI of the resource. */
	FString Uri;

	/** Display name of the resource. */
	FString Name;

	/** MIME type of the resource. */
	FString MimeType;

	/** Optional description. */
	FString Description;

	/** Parse from a JSON object. */
	static FMCPToolboxMCPResource FromJson(const TSharedPtr<FJsonObject>& JsonObj);
};

// ============================================================================
// MCP Client Class (FRunnable-based worker thread)
// ============================================================================

/**
 * TCP-based MCP client that connects to an MCP server using JSON-RPC 2.0 over raw TCP.
 * Implements FRunnable to run a dedicated worker thread for reading messages.
 */
class MCPTOOLBOX_API FMCPToolboxMCPClient : public FRunnable
{
public:
	FMCPToolboxMCPClient();
	virtual ~FMCPToolboxMCPClient();

	// ---- Connection Management ----

	/** Connect to the MCP server at the given host and port. */
	bool Connect(const FString& Host, int32 Port);

	/** Disconnect from the MCP server. */
	void Disconnect();

	/** Check if the client is currently connected. */
	bool IsConnected() const;

	/** Get the current connection state. */
	EMCPToolboxMCPClientState GetState() const;

	// ---- Request / Notification ----

	/**
	 * Send a JSON-RPC request and wait for the response.
	 * @param Method The MCP method to call.
	 * @param Params The parameters for the method.
	 * @param OutResponse The parsed JSON response.
	 * @return True if a valid response was received within the timeout.
	 */
	bool SendRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutResponse);

	/**
	 * Send a JSON-RPC notification (no response expected).
	 * @param Method The MCP notification method.
	 * @param Params The parameters for the notification.
	 * @return True if the notification was sent successfully.
	 */
	bool SendNotification(const FString& Method, const TSharedPtr<FJsonObject>& Params);

	// ---- MCP Protocol Operations ----

	/** Initialize the MCP session with the server. */
	bool Initialize();

	/** Ping the server to verify connectivity. */
	bool Ping();

	/**
	 * Call a tool on the MCP server.
	 * @param ToolName The name of the tool.
	 * @param Arguments The arguments for the tool.
	 * @param OutResult The result from the tool call.
	 * @return True on success.
	 */
	bool CallTool(const FString& ToolName, const TSharedPtr<FJsonObject>& Arguments, TSharedPtr<FJsonObject>& OutResult);

	/**
	 * List available tools from the MCP server.
	 * @param OutTools Array to receive tool definitions.
	 * @return True on success.
	 */
	bool ListTools(TArray<FMCPToolboxMCPTool>& OutTools);

	/**
	 * List available resources from the MCP server.
	 * @param OutResources Array to receive resource definitions.
	 * @return True on success.
	 */
	bool ListResources(TArray<FMCPToolboxMCPResource>& OutResources);

	/**
	 * Read a specific resource by URI.
	 * @param ResourceUri The URI of the resource.
	 * @param OutContent The resource content.
	 * @return True on success.
	 */
	bool ReadResource(const FString& ResourceUri, TSharedPtr<FJsonObject>& OutContent);

	// ---- Settings Accessors ----

	void SetConnectionTimeout(int32 TimeoutSeconds);
	void SetRequestTimeout(int32 TimeoutSeconds);
	void SetAutoReconnect(bool bAutoReconnect);
	void SetMaxReconnectAttempts(int32 MaxAttempts);
	void SetHeartbeatInterval(int32 IntervalSeconds);
	void SetEncryptionEnabled(bool bEnabled);
	void SetEncryptionKey(const FString& Key);

	// ---- Delegates ----

	FOnMCPToolboxConnected OnConnected;
	FOnMCPDisconnected OnDisconnected;
	FOnMCPError OnError;
	FOnMCPMessageReceived OnMessageReceived;
	FOnMCPToolDiscovered OnToolDiscovered;

protected:
	// ---- FRunnable Interface ----

	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	// ---- Internal Helpers ----

	/** Generate a unique message ID. */
	static int32 GenerateMessageId();

	/** Serialize a request to JSON-RPC 2.0 string. */
	FString SerializeRequest(const FString& Method, int32 Id, const TSharedPtr<FJsonObject>& Params, bool bIsNotification);

	/** Attempt to parse a JSON-RPC message from a raw buffer. Returns the parsed object on success. */
	bool ParseMessageFromBuffer(const FString& Buffer, TSharedPtr<FJsonObject>& OutMessage);

	/** Send raw data over the TCP socket. */
	bool SendRawData(const FString& Data);

	/** Encrypt outgoing data (if encryption is enabled). */
	FString EncryptData(const FString& PlainText) const;

	/** Decrypt incoming data (if encryption is enabled). */
	FString DecryptData(const FString& CipherText) const;

	/** Simple XOR obfuscation for encrypted transport. */
	FString ApplyCipher(const FString& Input) const;

	/** Execute the reconnection loop with exponential backoff. */
	bool PerformReconnect();

	/** Send heartbeat/ping to keep connection alive. */
	void SendHeartbeat();

	/** Handle an incoming JSON-RPC message (responses and notifications). */
	void ProcessIncomingMessage(const TSharedPtr<FJsonObject>& Message);

	// ---- State ----

	/** The TCP socket for communication. */
	FSocket* Socket;

	/** Thread for the worker (FRunnable). */
	FRunnableThread* WorkerThread;

	/** Atomic flag indicating the worker thread should continue running. */
	FThreadSafeBool bShouldRun;

	/** Atomic flag indicating the client is shutting down. */
	FThreadSafeBool bIsShuttingDown;

	/** Current connection state. */
	FThreadSafeCounter StateCounter; // 0=Disconnected, 1=Connecting, 2=Connected, 3=Error, 4=Reconnecting

	/** Atomic flag indicating if the client is currently connected. */
	FThreadSafeBool bIsConnected;

	// ---- Connection Settings ----

	/** Host address of the MCP server. */
	FString ServerHost;

	/** Port of the MCP server. */
	int32 ServerPort;

	/** Connection timeout in seconds. */
	int32 ConnectionTimeoutSeconds;

	/** Request timeout in seconds. */
	int32 RequestTimeoutSeconds;

	/** Whether auto-reconnect is enabled. */
	bool bAutoReconnect;

	/** Maximum number of reconnection attempts. */
	int32 MaxReconnectAttempts;

	/** Heartbeat interval in seconds. */
	int32 HeartbeatIntervalSeconds;

	/** Time of the last heartbeat. */
	double LastHeartbeatTime;

	/** Current reconnection attempt count. */
	int32 ReconnectAttempts;

	// ---- Encryption ----

	/** Whether message encryption is enabled. */
	bool bEncryptionEnabled;

	/** Encryption key/passphrase. */
	FString EncryptionKey;

	// ---- Synchronization ----

	/** Critical section for thread-safe socket operations. */
	FCriticalSection SocketCriticalSection;

	/** Critical section for the pending responses map. */
	FCriticalSection PendingResponsesCriticalSection;

	/** Critical section for the message batch queue. */
	FCriticalSection BatchQueueCriticalSection;

	/** Map of pending request IDs to their response promises. */
	TMap<int32, TFuture<TSharedPtr<FJsonObject>>> PendingResponses;

	/** Map of pending request IDs to their shared promises (for promise/future pattern). */
	TMap<int32, TSharedPtr<TPromise<TSharedPtr<FJsonObject>>>> PendingPromises;

	/** Counter for generating unique message IDs. */
	static int32 MessageIdCounter;

	/** Atomic for thread-safe ID generation. */
	static FThreadSafeCounter StaticMessageIdCounter;
};
