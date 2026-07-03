#ifndef CAPTIVE_DNS_H
#define CAPTIVE_DNS_H

// Starts a background task that answers every DNS query on UDP:53 with the
// SoftAP's own IP (192.168.4.1), so phones/laptops joining the setup AP
// auto-detect a captive portal. Best-effort only: browsing to
// http://192.168.4.1/ manually always works regardless.
void captive_dns_start(void);

#endif
