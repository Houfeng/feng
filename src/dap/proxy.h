#ifndef FENG_DAP_PROXY_H
#define FENG_DAP_PROXY_H

/* Launch the native DAP backend and transparently relay stdio bytes. */
int feng_dap_proxy_run(const char *backend_program,
                       int input_fd,
                       int output_fd,
                       int error_fd);

#endif /* FENG_DAP_PROXY_H */
