#pragma once
#include <stdint.h>
#include <stdbool.h>

// LAN-side camera audit. Requires the watch to be ASSOCIATED to a WiFi network
// (join via the WiFi tile first). Drives the ping sweep to enumerate live hosts,
// then TCP-probes each for camera services (RTSP 554/8554, camera HTTP UIs) and
// flags whether the video is exposed (RTSP DESCRIBE returns 200 without auth) or
// protected (401). Also surfaces any host whose MAC OUI is a known camera vendor.
//
// Intended for auditing networks you own or are authorised to assess.

enum CamAuditPhase {
    CA_IDLE = 0,
    CA_SWEEP,    // ping-sweeping the /24 for live hosts
    CA_PROBE,    // TCP-probing discovered hosts for camera services
    CA_DONE,
};

// Severity of a finding — drives colour + sort order in the UI.
enum CamLevel {
    CL_VENDOR = 1,   // camera-vendor OUI on the LAN, no open service found
    CL_HTTP   = 2,   // camera-looking HTTP UI reachable
    CL_RTSP_AUTH = 3,// RTSP present but DESCRIBE needs auth (401)
    CL_RTSP_OPEN = 4,// RTSP DESCRIBE returned 200 with no auth — stream exposed
    CL_DEFAULT_CREDS = 5, // a default username/password worked — fully owned
};

struct CamFinding {
    uint32_t    ip;          // host-order IPv4
    uint8_t     mac[6];
    bool        has_mac;
    const char *vendor;      // flock vendor string, or nullptr
    uint8_t     level;       // CamLevel
    uint16_t    port;        // the service port the finding is about (0 = none)
    char        note[40];    // e.g. "RTSP open :554", "HTTP :80", "OUI only"
};

// Starts the audit. Returns false if WiFi isn't associated.
bool cam_audit_start();
void cam_audit_stop();
bool cam_audit_is_running();

CamAuditPhase cam_audit_phase();
int cam_audit_scanned();     // hosts probed so far (PROBE phase)
int cam_audit_total();       // hosts to probe

// Pops the next queued finding into *out; returns false when the queue is empty.
bool cam_audit_next_finding(CamFinding *out);
