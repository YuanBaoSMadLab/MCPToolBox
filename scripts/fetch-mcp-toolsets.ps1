# ============================================================================
# fetch-mcp-toolsets.ps1
# ============================================================================
# 连接 UE5 MCP server，调用 list_toolsets + 所有 describe_toolset，
# 把结果写到 <ProjectDir>/.mcptoolbox/<toolset>.md
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File "fetch-mcp-toolsets.ps1" -ProjectDir "E:\MyUEProject"
#
# 参数：
#   -ProjectDir  UE5 项目根目录（含 .uproject 的目录），必填
#   -HostName    MCP server 主机，默认 127.0.0.1
#   -Port        MCP server 端口，默认 8000
#   -Force       覆盖已存在的 .mcptoolbox/ 目录
# ============================================================================

param(
    [Parameter(Mandatory = $true)]
    [string]$ProjectDir,
    [string]$HostName = "127.0.0.1",
    [int]$Port = 8000,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$BaseUrl = "http://${HostName}:${Port}/mcp"

if (-not (Test-Path $ProjectDir -PathType Container)) {
    Write-Error "项目目录不存在: $ProjectDir"
    exit 1
}

$OutputDir = Join-Path $ProjectDir ".mcptoolbox"
if ((Test-Path $OutputDir) -and -not $Force) {
    Write-Host "目标目录已存在: $OutputDir" -ForegroundColor Yellow
    Write-Host "使用 -Force 覆盖，或手动删除后重试。" -ForegroundColor Yellow
    exit 1
}
if (Test-Path $OutputDir) {
    Remove-Item -Path $OutputDir -Recurse -Force
}
New-Item -Path $OutputDir -ItemType Directory -Force | Out-Null

# ----------------------------------------------------------------------------
# HTTP 工具函数
# ----------------------------------------------------------------------------

function Send-JsonRpc {
    param(
        [int]$Id,
        [string]$Method,
        $Params,
        [string]$SessionId
    )

    $bodyObj = [ordered]@{
        jsonrpc = "2.0"
        id      = $Id
        method  = $Method
    }
    if ($null -ne $Params) {
        $bodyObj.params = $Params
    }
    $body = $bodyObj | ConvertTo-Json -Depth 20 -Compress

    $headers = @{
        "Content-Type"  = "application/json"
        "Accept"        = "application/json, text/event-stream"
    }
    if ($SessionId) { $headers["Mcp-Session-Id"] = $SessionId }

    try {
        $resp = Invoke-WebRequest -Uri $BaseUrl -Method POST -Headers $headers -Body $body -UseBasicParsing -TimeoutSec 30
    } catch {
        Write-Error "HTTP 请求失败 ($Method): $($_.Exception.Message)"
        return $null
    }

    # 更新 session id（服务器可能轮换）
    $newSid = $resp.Headers["Mcp-Session-Id"]
    if ($newSid) { $script:SessionId = $newSid }

    $content = $resp.Content
    $contentType = $resp.Headers["Content-Type"]

    # SSE 解析：按空行分隔事件，同一事件内多行 data: 用 \n 拼接
    if ($contentType -and $contentType.Contains("text/event-stream")) {
        $lines = $content -split "`r?`n"
        $dataAccumulator = ""
        $lastEventData = ""
        foreach ($line in $lines) {
            $trimmed = $line.Trim()
            if ($trimmed -eq "") {
                # 事件边界
                if ($dataAccumulator) {
                    $lastEventData = $dataAccumulator
                    $dataAccumulator = ""
                }
            } elseif ($trimmed.StartsWith("data:")) {
                $dataPart = $trimmed.Substring(5).TrimStart()
                if ($dataAccumulator) { $dataAccumulator += "`n" }
                $dataAccumulator += $dataPart
            }
        }
        # 若最后没有空行结尾，仍要保留累积的 data
        if ($dataAccumulator -and -not $lastEventData) {
            $lastEventData = $dataAccumulator
        }
        $content = $lastEventData
    }

    try {
        return ($content | ConvertFrom-Json)
    } catch {
        Write-Error "JSON 解析失败 ($Method): $($_.Exception.Message)`nContent: $($content.Substring(0, [Math]::Min(500, $content.Length)))"
        return $null
    }
}

function Get-TextFromContent {
    param($Result)
    # MCP tools/call 返回 {content: [{type: "text", text: "..."}]}
    if ($null -eq $Result) { return $null }
    if ($Result.PSObject.Properties.Name -contains "content") {
        foreach ($item in $Result.content) {
            if ($item.type -eq "text" -and $item.text) {
                return $item.text
            }
        }
    }
    # Fallback: 直接是字符串
    if ($Result -is [string]) { return $Result }
    return $null
}

# ----------------------------------------------------------------------------
# Step 1: Initialize
# ----------------------------------------------------------------------------

Write-Host "[1/4] 初始化 MCP 会话..." -ForegroundColor Cyan
$script:SessionId = $null
$initParams = [ordered]@{
    protocolVersion = "2024-11-05"
    capabilities    = @{}
    clientInfo      = [ordered]@{ name = "fetch-mcp-toolsets"; version = "1.0" }
}
$initResp = Send-JsonRpc -Id 1 -Method "initialize" -Params $initParams -SessionId $null
if ($null -eq $initResp) {
    Write-Error "initialize 失败"
    exit 1
}
if (-not $script:SessionId) {
    Write-Error "未获取到 Mcp-Session-Id（服务器可能不支持 streamable HTTP）"
    exit 1
}
Write-Host "      Session: $($script:SessionId)" -ForegroundColor Green

# 发送 initialized 通知
$notifyBody = [ordered]@{
    jsonrpc = "2.0"
    method  = "notifications/initialized"
} | ConvertTo-Json -Depth 5 -Compress
try {
    $notifyHeaders = @{
        "Content-Type"  = "application/json"
        "Mcp-Session-Id" = $script:SessionId
    }
    Invoke-WebRequest -Uri $BaseUrl -Method POST -Headers $notifyHeaders -Body $notifyBody -UseBasicParsing -TimeoutSec 10 | Out-Null
} catch {
    Write-Warning "notifications/initialized 发送失败（忽略）: $($_.Exception.Message)"
}

# ----------------------------------------------------------------------------
# Step 2: list_toolsets
# ----------------------------------------------------------------------------

Write-Host "[2/4] 调用 list_toolsets..." -ForegroundColor Cyan
$listParams = [ordered]@{
    name = "list_toolsets"
}
$listResp = Send-JsonRpc -Id 2 -Method "tools/call" -Params $listParams -SessionId $script:SessionId
if ($null -eq $listResp) {
    Write-Error "list_toolsets 失败"
    exit 1
}

$listText = Get-TextFromContent -Result $listResp.result
$toolsetNames = @()
if ($listText) {
    try {
        $parsed = $listText | ConvertFrom-Json
        if ($parsed -is [System.Array]) {
            foreach ($item in $parsed) {
                if ($item -is [string]) {
                    $toolsetNames += $item
                } elseif ($item.PSObject.Properties.Name -contains "name") {
                    $toolsetNames += $item.name
                }
            }
        }
    } catch {
        # 可能是文本格式 "- name: desc"
        $lines = $listText -split "`r?`n"
        foreach ($line in $lines) {
            $t = $line.Trim()
            if ($t.StartsWith("- ") -or $t.StartsWith("* ")) { $t = $t.Substring(2).Trim() }
            $colonIdx = $t.IndexOf(':')
            if ($colonIdx -gt 0) { $t = $t.Substring(0, $colonIdx).Trim() }
            if ($t -and $t -notmatch '^\s*$') { $toolsetNames += $t }
        }
    }
}

if ($toolsetNames.Count -eq 0) {
    Write-Error "未获取到任何 toolset"
    Write-Host "原始响应:" -ForegroundColor Yellow
    Write-Host $listText -ForegroundColor Gray
    exit 1
}
Write-Host "      发现 $($toolsetNames.Count) 个 toolset" -ForegroundColor Green

# ----------------------------------------------------------------------------
# Step 3: describe_toolset (并行)
# ----------------------------------------------------------------------------

Write-Host "[3/4] 调用 describe_toolset（并行 $([Math]::Min($toolsetNames.Count, 8)) 个）..." -ForegroundColor Cyan

# 简单串行调用（避免 session 冲突）
$toolsets = @{}
$requestId = 3
$totalTools = 0

foreach ($name in $toolsetNames) {
    $descParams = [ordered]@{
        name      = "describe_toolset"
        arguments = [ordered]@{ toolset_name = $name }
    }
    $descResp = Send-JsonRpc -Id $requestId -Method "tools/call" -Params $descParams -SessionId $script:SessionId
    $requestId++

    if ($null -eq $descResp) {
        Write-Warning "describe_toolset '$name' 失败，跳过"
        continue
    }

    $descText = Get-TextFromContent -Result $descResp.result
    $tools = @()
    if ($descText) {
        try {
            $parsed = $descText | ConvertFrom-Json
            if ($parsed -is [System.Array]) {
                $tools = $parsed
            } elseif ($parsed.PSObject.Properties.Name -contains "tools") {
                $tools = $parsed.tools
            } elseif ($parsed -is [object]) {
                $tools = @($parsed)
            }
        } catch {
            Write-Warning "解析 '$name' 工具列表失败: $($_.Exception.Message)"
        }
    }

    $toolsets[$name] = $tools
    $totalTools += $tools.Count
    Write-Host "      - $($name.PadRight(60)) $($tools.Count) 工具" -ForegroundColor Gray
}

Write-Host "      共 $totalTools 个工具" -ForegroundColor Green

# ----------------------------------------------------------------------------
# Step 4: 写 MD 文件
# ----------------------------------------------------------------------------

Write-Host "[4/4] 生成 MD 文件到 $OutputDir ..." -ForegroundColor Cyan

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

# 工具函数：把 toolset 名转为文件名（点替换为下划线，去掉特殊字符）
function Get-ToolsetFileName {
    param([string]$ToolsetName)
    return ($ToolsetName -replace '[^a-zA-Z0-9._-]', '_') + ".md"
}

# 工具函数：把 JSON Schema 类型转为可读描述
function Get-SchemaTypeDesc {
    param($Schema)
    if ($null -eq $Schema) { return "any" }
    if ($Schema.PSObject.Properties.Name -contains "type") {
        $t = $Schema.type
        if ($t -is [string]) { return $t }
        if ($t -is [array]) { return ($t -join "|") }
    }
    if ($Schema.PSObject.Properties.Name -contains "anyOf") { return "anyOf" }
    if ($Schema.PSObject.Properties.Name -contains "oneOf") { return "oneOf" }
    if ($Schema.PSObject.Properties.Name -contains "enum") {
        return "enum[" + ($Schema.enum -join "|") + "]"
    }
    return "any"
}

# 工具函数：生成单个工具的 MD 文档
function Build-ToolMd {
    param($Tool, [string]$ToolsetName)

    $toolName = $Tool.name
    $toolDesc = if ($Tool.PSObject.Properties.Name -contains "description") { $Tool.description } else { "" }
    $sb = [System.Text.StringBuilder]::new()

    [void]$sb.AppendLine("## ``$toolName``")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**完整名称:** ``$ToolsetName.$toolName``")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**描述:** $toolDesc")
    [void]$sb.AppendLine("")

    # 输入参数
    [void]$sb.AppendLine("### 输入参数")
    $inputSchema = $null
    if ($Tool.PSObject.Properties.Name -contains "inputSchema") {
        $inputSchema = $Tool.inputSchema
    }
    if ($null -ne $inputSchema -and $inputSchema.PSObject.Properties.Name -contains "properties") {
        $required = @()
        if ($inputSchema.PSObject.Properties.Name -contains "required" -and $inputSchema.required) {
            $required = @($inputSchema.required)
        }
        $props = $inputSchema.properties
        $propNames = @($props.PSObject.Properties.Name)
        if ($propNames.Count -eq 0) {
            [void]$sb.AppendLine("- (无参数)")
        } else {
            foreach ($pName in ($propNames | Sort-Object)) {
                $pSchema = $props.$pName
                $pType = Get-SchemaTypeDesc -Schema $pSchema
                $isRequired = if ($required -contains $pName) { "**必需**" } else { "可选" }
                $pDesc = if ($pSchema.PSObject.Properties.Name -contains "description") { $pSchema.description } else { "" }
                [void]$sb.AppendLine("- ``$pName`` ($pType) $isRequired" + $(if ($pDesc) { ": $pDesc" } else { "" }))
            }
        }
    } else {
        [void]$sb.AppendLine("- (无参数)")
    }
    [void]$sb.AppendLine("")

    # 调用代码
    [void]$sb.AppendLine("### 调用代码")
    [void]$sb.AppendLine("```json")
    $callObj = [ordered]@{
        toolset_name = $ToolsetName
        tool_name   = $toolName
        arguments   = @{}
    }
    # 填入参数示例骨架
    if ($null -ne $inputSchema -and $inputSchema.PSObject.Properties.Name -contains "properties") {
        $props = $inputSchema.properties
        $propNames = @($props.PSObject.Properties.Name)
        $sampleArgs = [ordered]@{}
        foreach ($pName in $propNames) {
            $pSchema = $props.$pName
            $pType = Get-SchemaTypeDesc -Schema $pSchema
            switch ($pType) {
                "string" { $sampleArgs[$pName] = "<string>" }
                "number" { $sampleArgs[$pName] = 0 }
                "integer" { $sampleArgs[$pName] = 0 }
                "boolean" { $sampleArgs[$pName] = $false }
                "array" { $sampleArgs[$pName] = @() }
                "object" { $sampleArgs[$pName] = @{} }
                default { $sampleArgs[$pName] = "<value>" }
            }
        }
        $callObj.arguments = $sampleArgs
    }
    $callJson = $callObj | ConvertTo-Json -Depth 10
    [void]$sb.AppendLine($callJson)
    [void]$sb.AppendLine("```")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("---")
    [void]$sb.AppendLine("")
    return $sb.ToString()
}

# 写每个 toolset 的 MD
foreach ($kv in $toolsets.GetEnumerator()) {
    $toolsetName = $kv.Key
    $tools = $kv.Value
    $fileName = Get-ToolsetFileName -ToolsetName $toolsetName
    $filePath = Join-Path $OutputDir $fileName

    $sb = [System.Text.StringBuilder]::new()
    [void]$sb.AppendLine("# $toolsetName")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("> $($tools.Count) 个工具 | 更新时间: $timestamp")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("**call_tool 参数:**")
    [void]$sb.AppendLine("- ``toolset_name`` (string) **必需**: ``$toolsetName``")
    [void]$sb.AppendLine("- ``tool_name`` (string) **必需**: 下方任一工具名")
    [void]$sb.AppendLine("- ``arguments`` (object): 见各工具的'输入参数'")
    [void]$sb.AppendLine("")
    [void]$sb.AppendLine("## 工具列表")
    [void]$sb.AppendLine("")
    foreach ($t in $tools) {
        $tName = $t.name
        $tDesc = if ($t.PSObject.Properties.Name -contains "description") { ($t.description -split "`r?`n")[0] } else { "" }
        [void]$sb.AppendLine("- [``$tName``](#$($tName.ToLower()))" + $(if ($tDesc) { " — $tDesc" } else { "" }))
    }
    [void]$sb.AppendLine("")

    foreach ($t in $tools) {
        [void]$sb.Append((Build-ToolMd -Tool $t -ToolsetName $toolsetName))
    }

    [System.IO.File]::WriteAllText($filePath, $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
    Write-Host "      写入: $fileName ($($tools.Count) 工具)" -ForegroundColor Gray
}

# 写 index.md
$indexSb = [System.Text.StringBuilder]::new()
[void]$indexSb.AppendLine("# UE5 MCP Toolsets 缓存索引")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("> 生成时间: $timestamp | Toolsets: $($toolsets.Count) | Tools: $totalTools")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("## Toolset 列表")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("| Toolset | 工具数 | 文档 |")
[void]$indexSb.AppendLine("|---------|--------|------|")
foreach ($kv in $toolsets.GetEnumerator()) {
    $name = $kv.Key
    $count = $kv.Value.Count
    $fileName = Get-ToolsetFileName -ToolsetName $name
    [void]$indexSb.AppendLine("| ``$name`` | $count | [$fileName]($fileName) |")
}
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("## 使用方法")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("LLM 调用 ``call_tool`` 时参考本目录下的 MD 文件：")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("1. 找到所需 toolset 的 MD 文件")
[void]$indexSb.AppendLine("2. 查看 ``tool_name`` 和参数 schema")
[void]$indexSb.AppendLine("3. 调用 ``call_tool(toolset_name=\"...\", tool_name=\"...\", arguments={...})``")
[void]$indexSb.AppendLine("")
[void]$indexSb.AppendLine("**关键规则**:")
[void]$indexSb.AppendLine("- ``toolset_name`` 是 toolset 的完整路径（如 ``editor_toolset.toolsets.material.MaterialTools``），**不含** ``tool_name``")
[void]$indexSb.AppendLine("- ``tool_name`` 是该 toolset 中的具体工具名（如 ``CreateMaterial``）")
[void]$indexSb.AppendLine("- 不要把 ``tool_name`` 拼进 ``toolset_name`` 里（常见错误）")
[void]$indexSb.AppendLine("- 不要调用 ``list_toolsets`` 或 ``describe_toolset``，本目录已包含所有信息")
[void]$indexSb.AppendLine("")

$indexPath = Join-Path $OutputDir "index.md"
[System.IO.File]::WriteAllText($indexPath, $indexSb.ToString(), [System.Text.UTF8Encoding]::new($false))
Write-Host "      写入: index.md" -ForegroundColor Gray

# 写 _meta.json（机器可读元数据）
$meta = [ordered]@{
    generated_at = $timestamp
    mcp_host     = $HostName
    mcp_port     = $Port
    session_id   = $script:SessionId
    toolsets     = @($toolsets.Keys | Sort-Object)
    total_tools  = $totalTools
}
$metaPath = Join-Path $OutputDir "_meta.json"
$meta | ConvertTo-Json -Depth 5 | Out-File -FilePath $metaPath -Encoding utf8
Write-Host "      写入: _meta.json" -ForegroundColor Gray

# ----------------------------------------------------------------------------
# 完成
# ----------------------------------------------------------------------------

Write-Host ""
Write-Host "✅ 完成！" -ForegroundColor Green
Write-Host "   输出目录: $OutputDir" -ForegroundColor Green
Write-Host "   Toolsets: $($toolsets.Count), Tools: $totalTools" -ForegroundColor Green
Write-Host ""
Write-Host "下一步:" -ForegroundColor Cyan
Write-Host "   1. 重启 UE5 编辑器（或重新加载 MCPToolbox 插件）以加载新的 .mcptoolbox/" -ForegroundColor White
Write-Host "   2. 在对话中提问，LLM 会自动读取这些 MD 并正确调用 call_tool" -ForegroundColor White
Write-Host ""
Write-Host "刷新缓存:" -ForegroundColor Cyan
Write-Host "   启用新 toolset 后重新运行此脚本（带 -Force 覆盖旧缓存）：" -ForegroundColor White
Write-Host "   powershell -ExecutionPolicy Bypass -File `"$PSCommandPath`" -ProjectDir `"$ProjectDir`" -Force" -ForegroundColor Gray
