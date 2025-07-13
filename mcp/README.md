# Sogen MCP Server

A Model Context Protocol (MCP) server that provides AI access to the sogen userspace emulator.

## Setup

1. Install dependencies:
   ```bash
   npm install
   ```

2. Run the server:
   ```bash
   npm start
   ```

   Or for development with auto-restart:
   ```bash
   npm run dev
   ```

## Structure

- `server.js` - Main server implementation
- `package.json` - Node.js project configuration

## Available Tools

### `list_applications`
Lists all available applications in the sogen userspace emulator.

**Parameters:** None

**Example usage:**
```json
{
  "name": "list_applications",
  "arguments": {}
}
```

### `run_application`
Executes a specific application from the allowed list in the sogen userspace emulator.

**Parameters:**
- `application` (string, required): The name of the application to run (use `list_applications` to see available apps)
- `args` (array of strings, optional): Arguments to pass to the application
- `timeout` (number, optional): Timeout in milliseconds (default: 5000)

**Example usage:**
```json
{
  "name": "run_application",
  "arguments": {
    "application": "echo",
    "args": ["Hello from sogen!"],
    "timeout": 3000
  }
}
```

## Adding More Tools

To add additional tools:
1. Add the tool definition to the `ListToolsRequestSchema` handler
2. Add the implementation to the `CallToolRequestSchema` handler
3. Create the corresponding method in the `MyMCPServer` class

## Execution Model

The server uses an **analyzer-based execution model**:

- Applications are not executed directly
- Instead, the server runs: `C:/analyer.exe -e C:/somedir <application_name> [args...]`
- The analyzer handles the actual execution within the sogen environment
- All output comes from the analyzer process

### Command Structure
```
C:/analyer.exe -e C:/somedir <application_name> [arguments...]
```

Where:
- `C:/analyer.exe` - The sogen analyzer executable
- `-e C:/somedir` - Analyzer flags and environment directory
- `<application_name>` - The application from `get_applications()`
- `[arguments...]` - Optional arguments passed to the application

## Implementation Required

You need to provide the implementation for the `get_applications()` method in `server.js`. This method should:

```javascript
async get_applications() {
  // Return a Promise that resolves to a string array
  // Example: return ['echo', 'ls', 'cat', 'grep'];
}
```

## Usage

This server allows AI assistants to:
1. **List available applications** using `list_applications`
2. **Execute specific applications** using `run_application` with validation

The server communicates over stdio and is designed for MCP-compatible clients.

### Example Workflow
1. Call `list_applications` to see what's available
2. Call `run_application` with a valid application name and arguments

## Next Steps

1. **Implement `get_applications()` method** - Provide the actual implementation
2. Add application-specific argument validation
3. Implement resource handling for file access
4. Add comprehensive logging and monitoring
5. Consider per-application sandboxing for enhanced security
