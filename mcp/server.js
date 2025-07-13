#!/usr/bin/env node

import { Server } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import {
  CallToolRequestSchema,
  ListToolsRequestSchema,
} from "@modelcontextprotocol/sdk/types.js";
import { spawn } from "child_process";
import path from "path";
import { fileURLToPath } from "url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

class SogenMCPServer {
  constructor() {
    this.server = new Server(
      {
        name: "sogen-mcp-server",
        version: "1.0.0",
      },
      {
        capabilities: {
          tools: {},
        },
      }
    );

    this.setupToolHandlers();
    this.setupErrorHandling();
  }

  setupToolHandlers() {
    // Handle tool listing
    this.server.setRequestHandler(ListToolsRequestSchema, async () => {
      return {
        tools: [
          {
            name: "list_applications",
            description:
              "List all available applications in the sogen userspace emulator",
            inputSchema: {
              type: "object",
              properties: {},
              additionalProperties: false,
            },
          },
          {
            name: "run_application",
            description:
              "Execute a specific application in the sogen userspace emulator",
            inputSchema: {
              type: "object",
              properties: {
                application: {
                  type: "string",
                  description:
                    "The name of the application to run (use list_applications to see available apps)",
                },
                args: {
                  type: "array",
                  items: {
                    type: "string",
                  },
                  description: "Arguments to pass to the application",
                  default: [],
                },
                timeout: {
                  type: "number",
                  description: "Timeout in milliseconds (default: 50000)",
                  default: 50000,
                },
              },
              required: ["application"],
            },
          },
        ],
      };
    });

    // Handle tool execution
    this.server.setRequestHandler(CallToolRequestSchema, async (request) => {
      const { name, arguments: args } = request.params;

      switch (name) {
        case "list_applications":
          return await this.listApplications();

        case "run_application":
          return await this.runApplication(
            args.application,
            args.args || [],
            args.timeout || 50000
          );

        default:
          throw new Error(`Unknown tool: ${name}`);
      }
    });
  }

  async listApplications() {
    try {
      const applications = await this.get_applications();

      return {
        content: [
          {
            type: "text",
            text: `Available applications in sogen:\n\n${applications
              .map((app) => `• ${app}`)
              .join("\n")}\n\nTotal: ${applications.length} application(s)`,
          },
        ],
      };
    } catch (error) {
      return {
        content: [
          {
            type: "text",
            text: `Error listing applications: ${error.message}`,
          },
        ],
      };
    }
  }

  async runApplication(applicationName, args = [], timeout = 5000) {
    try {
      // First, get the list of available applications to validate
      const availableApps = await this.get_applications();

      if (!availableApps.includes(applicationName)) {
        return {
          content: [
            {
              type: "text",
              text: `Error: Application '${applicationName}' is not available.\n\nAvailable applications:\n${availableApps
                .map((app) => `• ${app}`)
                .join("\n")}`,
            },
          ],
        };
      }

      return await this.executeApplication(applicationName, args, timeout);
    } catch (error) {
      return {
        content: [
          {
            type: "text",
            text: `Error running application '${applicationName}': ${error.message}`,
          },
        ],
      };
    }
  }

  async executeApplication(applicationName, args, timeout) {
    return new Promise((resolve, reject) => {
      // Get the parent directory (emulator root)
      const emulatorRoot = path.dirname(__dirname);

      // Build the analyzer command: C:/analyer.exe -e C:/somedir <application_name>
      const analyzerPath =
        "C:\\Users\\mauri\\Desktop\\emulator\\build\\vs2022\\artifacts-relwithdebinfo\\analyzer.exe";
      const analyzerArgs = [
        "-c",
        "-e",
        "C:\\Users\\mauri\\Desktop\\emulator\\src\\tools\\root",
        applicationName,
        ...args,
      ];

      const child = spawn(analyzerPath, analyzerArgs, {
        cwd: "C:\\Users\\mauri\\Desktop\\emulator\\build\\vs2022\\artifacts-relwithdebinfo",
        shell: true,
        stdio: ["pipe", "pipe", "pipe"],
      });

      let stdout = "";
      let stderr = "";
      let timedOut = false;

      // Set up timeout
      const timer = setTimeout(() => {
        timedOut = true;
        child.kill("SIGTERM");
      }, timeout);

      // Collect stdout
      child.stdout.on("data", (data) => {
        stdout += data.toString();
      });

      // Collect stderr
      child.stderr.on("data", (data) => {
        stderr += data.toString();
      });

      // Handle process completion
      child.on("close", (code) => {
        clearTimeout(timer);

        if (timedOut) {
          resolve({
            content: [
              {
                type: "text",
                text: `Application '${applicationName}' timed out after ${timeout}ms\nAnalyzer command: ${analyzerPath} ${analyzerArgs.join(
                  " "
                )}\nPartial stdout: ${stdout}\nPartial stderr: ${stderr}`,
              },
            ],
          });
        } else {
          const output = [];

          if (stdout) {
            output.push(`STDOUT:\n${stdout}`);
          }

          if (stderr) {
            output.push(`STDERR:\n${stderr}`);
          }

          if (!stdout && !stderr) {
            output.push("Application executed successfully with no output.");
          }

          output.push(`Exit code: ${code}`);

          resolve({
            content: [
              {
                type: "text",
                text: `Application: ${applicationName}\nArguments: [${args.join(
                  ", "
                )}]\nAnalyzer command: ${analyzerPath} ${analyzerArgs.join(
                  " "
                )}\n\n${output.join("\n\n")}`,
              },
            ],
          });
        }
      });

      // Handle spawn errors
      child.on("error", (error) => {
        clearTimeout(timer);
        resolve({
          content: [
            {
              type: "text",
              text: `Error executing application '${applicationName}' via analyzer: ${
                error.message
              }\nAnalyzer command: ${analyzerPath} ${analyzerArgs.join(" ")}`,
            },
          ],
        });
      });
    });
  }

  async get_applications() {
    return [
      "c:/test-sample.exe",
      "c:/wukong/b1/Binaries/Win64/b1-Win64-Shipping.exe",
    ];
  }

  setupErrorHandling() {
    this.server.onerror = (error) => {
      console.error("[MCP Error]", error);
    };

    process.on("SIGINT", async () => {
      await this.server.close();
      process.exit(0);
    });
  }

  async run() {
    const transport = new StdioServerTransport();
    await this.server.connect(transport);
    console.error("MCP Server running on stdio");
  }
}

// Start the server
const server = new SogenMCPServer();
server.run().catch((error) => {
  console.error("Failed to start server:", error);
  process.exit(1);
});
