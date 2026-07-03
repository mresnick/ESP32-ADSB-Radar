#ifndef PROVISIONING_HTTP_SERVER_H
#define PROVISIONING_HTTP_SERVER_H

// Starts the config-form HTTP server on port 80. GET "/" (and common OS
// captive-portal probe paths) serves the form; POST "/save" validates and
// persists the submitted config via config_store, then reboots.
void provisioning_http_server_start(void);

#endif
