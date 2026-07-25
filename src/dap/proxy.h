#ifndef FENG_DAP_PROXY_H
#define FENG_DAP_PROXY_H

/*
 * Resolve the native DAP backend after launch validation succeeds.
 * The returned path and optional error message are owned by the proxy.
 */
typedef char *(*FengDapBackendResolver)(void *context, char **out_error_message);

/* Validate launch, resolve the native backend, and relay DAP stdio messages. */
int feng_dap_proxy_run(FengDapBackendResolver backend_resolver,
                       void *backend_resolver_context,
                       int input_fd,
                       int output_fd,
                       int error_fd);

#endif /* FENG_DAP_PROXY_H */
