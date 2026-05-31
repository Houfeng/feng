use zed_extension_api as zed;

struct FengExtension;

impl FengExtension {
    fn resolve_feng_binary(worktree: &zed::Worktree) -> Result<String, String> {
        worktree
            .which("feng")
            .ok_or_else(|| "feng executable was not found in PATH".to_string())
    }

    fn parse_debug_config(config_json: &str) -> serde_json::Value {
        serde_json::from_str(config_json).unwrap_or_else(|_| serde_json::json!({}))
    }
}

impl zed::Extension for FengExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let feng = Self::resolve_feng_binary(worktree)?;

        Ok(zed::Command {
            command: feng,
            args: vec!["lsp".to_string(), "--stdio".to_string()],
            env: worktree.shell_env(),
        })
    }

    fn get_dap_binary(
        &mut self,
        adapter_name: String,
        config: zed::DebugTaskDefinition,
        user_provided_debug_adapter_path: Option<String>,
        worktree: &zed::Worktree,
    ) -> Result<zed::DebugAdapterBinary, String> {
        if adapter_name != "feng" {
            return Err(format!("unsupported debug adapter: {adapter_name}"));
        }

        let command = match user_provided_debug_adapter_path {
            Some(path) if !path.is_empty() => path,
            _ => Self::resolve_feng_binary(worktree)?,
        };

        let parsed_config = Self::parse_debug_config(&config.config);
        let request = self.dap_request_kind(adapter_name, parsed_config)?;

        Ok(zed::DebugAdapterBinary {
            command: Some(command),
            arguments: vec!["dap".to_string(), "--stdio".to_string()],
            envs: worktree.shell_env(),
            cwd: Some(worktree.root_path()),
            connection: None,
            request_args: zed::StartDebuggingRequestArguments {
                configuration: config.config.to_string(),
                request,
            },
        })
    }

    fn dap_request_kind(
        &mut self,
        adapter_name: String,
        config: serde_json::Value,
    ) -> Result<zed::StartDebuggingRequestArgumentsRequest, String> {
        if adapter_name != "feng" {
            return Err(format!("unsupported debug adapter: {adapter_name}"));
        }

        let request = config
            .get("request")
            .and_then(|value| value.as_str())
            .unwrap_or("launch");

        match request {
            "launch" => Ok(zed::StartDebuggingRequestArgumentsRequest::Launch),
            "attach" => Ok(zed::StartDebuggingRequestArgumentsRequest::Attach),
            other => Err(format!("unsupported debug request kind: {other}")),
        }
    }
}

zed::register_extension!(FengExtension);
