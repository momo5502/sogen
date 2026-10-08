param(
    [string]$ServerUri = 'ws://127.0.0.1:4500',
    [string]$Prompt = 'Run the Windows batch command cmd /c dir /b /a-d in your current working directory. Print only the file names it outputs, one per line, with no formatting or commentary. Do not change directories or infer the contents without running the command.',
    [int]$TimeoutSeconds = 60,
    [switch]$Ephemeral
)

$ErrorActionPreference = 'Stop'
$socket = [System.Net.WebSockets.ClientWebSocket]::new()
$cancel = [System.Threading.CancellationToken]::None
$requestId = 0

function Send-Rpc([string]$Method, [hashtable]$Params) {
    $script:requestId++
    $message = @{
        jsonrpc = '2.0'
        id = $script:requestId
        method = $Method
        params = $Params
    } | ConvertTo-Json -Depth 30 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($message)
    $segment = [System.ArraySegment[byte]]::new($bytes)
    $null = $script:socket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $script:cancel).GetAwaiter().GetResult()
    return $script:requestId
}

function Receive-Message {
    $buffer = [byte[]]::new(65536)
    $builder = [System.Text.StringBuilder]::new()
    $receiveCancellation = [System.Threading.CancellationTokenSource]::new($script:TimeoutSeconds * 1000)
    do {
        $segment = [System.ArraySegment[byte]]::new($buffer)
        try {
            $result = $script:socket.ReceiveAsync($segment, $receiveCancellation.Token).GetAwaiter().GetResult()
        }
        catch [System.OperationCanceledException] {
            throw "Timed out after $script:TimeoutSeconds seconds waiting for an app-server message."
        }
        if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
            throw 'App server closed the WebSocket.'
        }
        [void]$builder.Append([System.Text.Encoding]::UTF8.GetString($buffer, 0, $result.Count))
    } while (-not $result.EndOfMessage)
    $receiveCancellation.Dispose()
    return $builder.ToString() | ConvertFrom-Json
}

function Receive-Response($Id) {
    while ($true) {
        $message = Receive-Message
        if ($message.id -eq $Id) {
            if ($message.error) {
                throw "RPC $Id failed: $($message.error | ConvertTo-Json -Compress -Depth 10)"
            }
            return $message.result
        }
    }
}

try {
    $connectCancellation = [System.Threading.CancellationTokenSource]::new($TimeoutSeconds * 1000)
    try {
        $null = $socket.ConnectAsync([uri]$ServerUri, $connectCancellation.Token).GetAwaiter().GetResult()
    }
    catch [System.OperationCanceledException] {
        throw "Timed out after $TimeoutSeconds seconds connecting to $ServerUri."
    }
    finally {
        $connectCancellation.Dispose()
    }
    Write-Host "Connected to $ServerUri"

    $id = Send-Rpc 'initialize' @{
        clientInfo = @{ name = 'sogen-app-server-smoke'; title = 'Sogen app-server smoke'; version = '1.0.0' }
    }
    $null = Receive-Response $id
    Write-Host 'Initialized app server'
    $initialized = @{ jsonrpc = '2.0'; method = 'initialized'; params = @{} } | ConvertTo-Json -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($initialized)
    $segment = [System.ArraySegment[byte]]::new($bytes)
    $null = $socket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, $cancel).GetAwaiter().GetResult()

    $threadParams = @{ cwd = (Get-Location).Path }
    $expectedFiles = @(Get-ChildItem -LiteralPath $threadParams.cwd -File -Force | ForEach-Object Name)
    if ($Ephemeral) {
        $threadParams.ephemeral = $true
    }
    $id = Send-Rpc 'thread/start' $threadParams
    $thread = Receive-Response $id
    $threadId = $thread.thread.id
    Write-Host "Started thread $threadId in $($threadParams.cwd)"

    $id = Send-Rpc 'turn/start' @{
        threadId = $threadId
        input = @(@{ type = 'text'; text = $Prompt })
    }
    $null = Receive-Response $id
    Write-Host 'Started turn'

    $output = [System.Text.StringBuilder]::new()
    while ($true) {
        $message = Receive-Message
        if ($message.method -eq 'item/agentMessage/delta') {
            [void]$output.Append($message.params.delta)
        }
        if ($message.method -eq 'turn/completed') {
            $status = $message.params.turn.status
            Write-Host "Turn completed with status $status"
            if ($status -ne 'completed') {
                throw "Turn ended with status '$status'."
            }
            $response = $output.ToString().Trim()
            if (-not $response) {
                throw 'Agent completed the turn without a response.'
            }
            $listedFiles = @($response -split '\r?\n' | ForEach-Object { $_.Trim() })
            $missingFiles = @($expectedFiles | Where-Object { $listedFiles -notcontains $_ })
            if ($missingFiles.Count -gt 0) {
                throw "Agent did not list $($missingFiles.Count) file(s), including: $((@($missingFiles | Select-Object -First 5) -join ', ')). Response: '$response'"
            }
            Write-Output $response
            break
        }
    }
}
finally {
    if ($socket.State -eq [System.Net.WebSockets.WebSocketState]::Open) {
        $null = $socket.CloseOutputAsync([System.Net.WebSockets.WebSocketCloseStatus]::NormalClosure, 'done', $cancel).GetAwaiter().GetResult()
    }
    $socket.Dispose()
}
