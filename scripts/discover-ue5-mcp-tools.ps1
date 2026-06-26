#!/usr/bin/env pwsh
<#
.SYNOPSIS
  UE5 MCP 全工具集探测脚本 — 输出为 MD 文件用于系统提示词缓存
.DESCRIPTION
  连接 UE 编辑器 MCP (127.0.0.1:8000)，自动发现所有工具集和工具，
  整理为 Markdown 文件，包含参数说明和示例。
  UE 编辑器必须先启动且 MCP 服务正在运行（默认 8000 端口）。
.PARAMETER Port
  MCP 端口，默认 8000
.PARAMETER OutDir
  输出目录，默认当前脚本目录
#>

param(
    [int]$Port = 8000,
    [string]$OutDir = $PSScriptRoot
)

$ErrorActionPreference = "Stop"
$BaseUrl = "http://127.0.0.1:$Port/mcp"
$SessionId = $null

# ---- Helpers ----
function Send-McpRequest {
    param([string]$Method, [object]$Params = @{}, [int]$TimeoutSec = 30)
    $id = Get-Random -Minimum 1000 -Maximum 99999
    $body = @{ jsonrpc = "2.0"; id = $id; method = $Method; params = $Params } | ConvertTo-Json -Depth 10 -Compress

    $headers = @{ "Content-Type" = "application/json" }
    if ($SessionId) { $headers["Mcp-Session-Id"] = $SessionId }

    try {
        $resp = Invoke-WebRequest -Uri $BaseUrl -Method Post -Body $body -Headers $headers `
            -ContentType "application/json" -TimeoutSec $TimeoutSec -SkipHttpErrorCheck
    } catch { return $null }

    $sid = $resp.Headers["Mcp-Session-Id"]
    if ($sid) { $Script:SessionId = $sid }

    $ct = if ($resp.Headers["Content-Type"]) { $resp.Headers["Content-Type"] } else { "" }
    $raw = $resp.Content

    if ($ct -match "application/json") {
        return ($raw | ConvertFrom-Json)
    }
    if ($ct -match "text/event-stream") {
        # Parse SSE: find the first data: line with result.content
        $lines = $raw -split "`r?`n"
        $eventData = ""
        foreach ($line in $lines) {
            $t = $line.TrimEnd()
            if ($t -match "^data:\s*(.+)") { $eventData = $Matches[1] }
            elseif ($t -eq "" -and $eventData) { break }
        }
        if ($eventData) {
            $parsed = $eventData | ConvertFrom-Json
            if ($parsed.result.content) {
                return ($parsed.result.content | Where-Object { $_.type -eq "text" } | ForEach-Object { $_.text }) -join "`n"
            }
            return $parsed.result
        }
        return $raw
    }
    return $raw
}

Write-Host "=" * 70
Write-Host "  UE5 MCP 全工具集探测 (port $Port)"
Write-Host "=" * 70

# Initialize
Write-Host "`n[1/4] 初始化 MCP 会话..."
$init = Send-McpRequest -Method "initialize" -Params @{
    protocolVersion = "2025-11-25"
    capabilities = @{}
    clientInfo = @{ name = "ue5-tool-discover"; version = "1.0" }
}
Write-Host "       Session: $SessionId"

# Get tools/list first
Write-Host "`n[2/4] 获取顶层工具..."
$toolsResult = Send-McpRequest -Method "tools/list"
Write-Host "       顶层工具: $($toolsResult.tools.Count)"

# Get all toolsets via call_tool(list_toolsets)
Write-Host "`n[3/4] 获取工具集列表..."
$listText = Send-McpRequest -Method "tools/call" -Params @{ name = "list_toolsets"; arguments = @{} }
if (-not $listText) { Write-Error "无法获取工具集列表，请确认 UE 编辑器和 MCP 服务已启动"; exit 1 }

Write-Host "       原始输出: $($listText.Substring(0, [Math]::Min(200, $listText.Length)))..."

# Parse toolset names
$tsNames = @()
foreach ($line in ($listText -split "`n")) {
    $trimmed = $line.Trim() -replace '^- ', ''
    if ($trimmed -match '^(\S+):') {
        $tsNames += $Matches[1]
    }
}
Write-Host "       解析到 $($tsNames.Count) 个工具集"

# Describe each toolset
Write-Host "`n[4/4] 获取每个工具集详情..."
$allToolsets = @{}
$totalTools = 0

foreach ($tsName in $tsNames) {
    Write-Host "       describe: $tsName..."
    $desc = Send-McpRequest -Method "tools/call" -Params @{ name = "describe_toolset"; arguments = @{ toolset_name = $tsName } }
    if (-not $desc) { Write-Host "         !! 失败"; continue }

    $tools = @()
    # Parse the describe output — typically each line is "- toolName: description"
    foreach ($line in ($desc -split "`n")) {
        $trimmed = $line.Trim() -replace '^- ', ''
        if ($trimmed -match '^(\S+)\s*:\s*(.*)$') {
            $tools += @{ name = $Matches[1]; desc = $Matches[2] }
        } elseif ($trimmed -match '^(\S+)$') {
            $tools += @{ name = $Matches[1]; desc = "" }
        }
    }
    $allToolsets[$tsName] = $tools
    $totalTools += $tools.Count
}

Write-Host "`n       总计: $($allToolsets.Count) 工具集, $totalTools 工具"

# ---- Generate MD files ----
$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

# 1. Full toolset catalog
$catalogPath = Join-Path $OutDir "ue5-mcp-tools-catalog.md"
$catalog = @"
# UE5 MCP 工具集目录

> 自动生成时间: $timestamp
> 工具集数: $($allToolsets.Count) | 工具总数: $totalTools

## 已发现MCP工具

直接调用 `call_tool` 即可。格式: `call_tool(toolset_name="完整工具集名", tool_name="简短工具名", arguments={...})`

"@

foreach ($tsName in ($allToolsets.Keys | Sort-Object)) {
    $tools = $allToolsets[$tsName]
    $shortName = $tsName -replace '.*\.' , ''
    $catalog += "### $tsName`n`n"
    $catalog += "| 工具名 | 说明 |`n"
    $catalog += "|--------|------|`n"
    foreach ($tool in $tools) {
        $catalog += "| ``$($tool.name)`` | $($tool.desc) |`n"
    }
    $catalog += "`n"
}
$catalog | Out-File -FilePath $catalogPath -Encoding UTF8
Write-Host "`n已生成: $catalogPath"

# 2. Quick reference (compact, for inclusion in system prompt)
$quickPath = Join-Path $OutDir "ue5-mcp-tools-quick.md"
$quick = @"
## 已发现MCP工具

$totalTools 个工具如下。直接通过 `call_tool(toolset_name="...", tool_name="...", arguments={...})` 调用。

"@
foreach ($tsName in ($allToolsets.Keys | Sort-Object)) {
    $tools = $allToolsets[$tsName]
    $quick += "**$tsName**: "
    $names = ($tools | ForEach-Object { $_.name }) -join ", "
    $quick += "$names`n"
}
$quick | Out-File -FilePath $quickPath -Encoding UTF8
Write-Host "已生成: $quickPath"

# 3. Per-toolset detail files
$detailDir = Join-Path $OutDir "ue5-mcp-toolsets"
New-Item -ItemType Directory -Force -Path $detailDir | Out-Null
foreach ($tsName in ($allToolsets.Keys | Sort-Object)) {
    $tools = $allToolsets[$tsName]
    $shortName = $tsName -replace '.*\.' , ''
    $detail = @"
# $tsName

$($tools.Count) 个工具

| 工具名 | 说明 |
|--------|------|
"@
    foreach ($tool in $tools) {
        $detail += "| ``$($tool.name)`` | $($tool.desc) |`n"
    }
    $detailPath = Join-Path $detailDir "$shortName.md"
    $detail | Out-File -FilePath $detailPath -Encoding UTF8
}
Write-Host "已生成: $detailDir\ ($($allToolsets.Count) 个工具集文件)"

Write-Host "`n完成！共 $totalTools 个工具，$($allToolsets.Count) 个工具集"
Write-Host "  $catalogPath"
Write-Host "  $quickPath"
Write-Host "  $detailDir\"
