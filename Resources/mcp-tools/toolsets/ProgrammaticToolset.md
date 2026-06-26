# editor_toolset.toolsets.programmatic.ProgrammaticToolset

2 个工具

| 工具名 | 说明 |
|--------|------|
| `execute_tool_script` | Execute a Python script against the toolset APIs.

        Use this to batch multiple tool calls into a single script execution,
        reducing round-trips and context usage.

        IMPORTANT: Available modules and usage instructions are described by the
        value returned by `get_execution_environment`. You MUST call
        `get_execution_environment` once in the conversation before using this
        tool. Read the value in the `instructions` field in the returned
        environment info prior to calling this function, so that you understand
        what APIs are available and how to use them.

        Before writing a script that calls multiple tools, look up the output
        schemas (if available) for any tools you plan to use. This returns the
        JSON schema describing each tool's return value, so you know how to
        parse results and pass data between calls.

        Args:
            script: Python script to execute. Must define a `run()` function
                that returns a `Dict[str, Any]`.

        Returns:
            JSON-encoded dict returned by the script's `run()` function.

        Raises:
            SyntaxError: If the script has invalid syntax.
            ValueError: If the script imports a disallowed module or does not
                define a `run()` function.
            TypeError: If `run()` does not return a dict.
            Exception: Any unhandled exception raised by the script. |
| `get_execution_environment` | Get details about execution environment.

        This includes instructions on how to write scripts, and constraints,
        such as what modules may be imported and the script entrypoint and
        function signature.

        Returns:
            Returns the current execution environment. |