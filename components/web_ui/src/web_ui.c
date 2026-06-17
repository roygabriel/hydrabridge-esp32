/* Web UI: status page + REST endpoints. WiFi-served status, BLE scan,
 * light registration, commands (incl. full channels, profiles), OTA.
 * ESP-IDF only.
 *
 * Endpoints:
 *   GET  /                          HTML with Scan / Add / On / Off / Profiles
 *   GET  /api/status                JSON: uptime, light count, BLE state
 *   POST /api/scan                  kick off a 30s BLE scan (for pairing lights)
 *   GET  /api/scan/results          JSON list of discovered lights
 *   GET  /api/lights                JSON list of registered lights
 *   POST /api/lights                register a discovered light
 *   POST /api/lights/<id>/command   issue a light command (JSON body)
 *   GET  /api/logs                  recent event_log entries
 *   POST /api/ota                   firmware upload
 *   (profiles endpoints added in later phase)
 */

#ifdef ESP_PLATFORM

#include "web_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "ble_light_client.h"
#include "ble_scanner.h"
#include "ai_pump_protocol.h"
#include "command_queue.h"
#include "command_engine.h"
#include "event_log.h"
#include "light_registry.h"
#include "config_store.h"
#include "mqtt_bridge.h"
#include "modbus_interface.h"
#include "ota_update.h"
#include "pump_registry.h"
#include "pump_schedule_engine.h"
#include "schedule_engine.h"
#include "sun_service.h"
#include "time_service.h"
#include "wifi_station.h"

static const char *TAG = "web_ui";
static httpd_handle_t s_server = NULL;

#define SCAN_BUF_MAX        16
#define SCAN_DURATION_MS    30000  /* longer for pairing mode lights that may advertise sparsely or put key data in scan responses */

static ble_scan_result_t s_scan_buf[SCAN_BUF_MAX];
static size_t            s_scan_count;

static const char *channel_names[9] = {
    "brightness", "coolwhite", "blue", "deepred", "violet", "uv", "green", "royalblue", "moonlight"
};

static const user_profile_t k_builtin_profiles[] = {
    /* Values are 0..1000. Source percentages were multiplied by 10.
     * 105%/110% HD values are clamped to the current channel model max. */
    { "Zoa Pop",        "Best for zoas, mushrooms, LPS viewing, and fluorescence.",       { 1000, 150, 1000,  50, 1000,  950,  50, 1000,   0 } },
    { "AB Plus",        "Good balanced profile for mixed reef growth and color.",          { 1000, 250, 1000, 100,  900,  900, 100, 1000,   0 } },
    { "Mixed Reef",     "More natural look while still reef-safe.",                        { 1000, 400,  900, 100,  800,  750, 150,  900,   0 } },
    { "Frag Growth",    "Higher blue and violet, moderate white for frag racks.",          { 1000, 200, 1000,  50,  900,  850,  50, 1000,   0 } },
    { "Evening",        "After-work viewing without blasting PAR.",                        { 1000,  50,  650,   0,  550,  450,   0,  700,   0 } },
    { "Photo Mode",     "Better for coral photos with less overwhelming blue.",            { 1000, 450,  750,  80,  500,  400, 100,  750,   0 } },
    { "Moonlight Only", "Very low moonlight for a short night window.",                    { 1000,   0,    0,   0,    0,    0,   0,    0,  20 } },
};

static const size_t k_builtin_profile_count =
    sizeof(k_builtin_profiles) / sizeof(k_builtin_profiles[0]);

static const user_profile_t *builtin_profile_by_name(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < k_builtin_profile_count; ++i) {
        if (strcmp(k_builtin_profiles[i].name, name) == 0) {
            return &k_builtin_profiles[i];
        }
    }
    return NULL;
}

static void merge_scan_result(ble_scan_result_t *dst, const ble_scan_result_t *src)
{
    if (!dst || !src) return;
    dst->rssi = src->rssi;
    dst->ble_addr_type = src->ble_addr_type;
    if (src->name[0]) {
        strncpy(dst->name, src->name, sizeof dst->name - 1);
        dst->name[sizeof dst->name - 1] = '\0';
    }
    if (src->has_manuf_data) {
        dst->has_manuf_data = true;
        dst->manuf = src->manuf;
    }
    if (src->has_hydra_service) {
        dst->has_hydra_service = true;
        if (dst->name[0] == '\0') {
            strncpy(dst->name, "MOBIUS", sizeof dst->name - 1);
            dst->name[sizeof dst->name - 1] = '\0';
        }
    }
}

static void refresh_registered_peer(const registered_light_t *candidate, int8_t rssi)
{
    if (!candidate) return;
    const registered_light_t *existing = NULL;
    if (candidate->serial[0]) {
        existing = light_registry_get_by_serial(candidate->serial);
    }
    if (!existing) {
        existing = light_registry_get(candidate->light_id);
    }
    if (!existing) {
        existing = light_registry_get_by_addr(candidate->ble_addr);
    }
    if (existing &&
        light_registry_update_discovery(existing->light_id, candidate->ble_addr,
                                        candidate->ble_addr_type, rssi) == 0) {
        light_registry_save();
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* ===== HTML status / control page ===== */
static const char STATUS_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<title>HydraBridge ESP32</title>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<link rel=icon href=/favicon.ico>"
"<style>"
"*{box-sizing:border-box}"
":root{color-scheme:light;--bg:#f5f7fb;--panel:#fff;--ink:#172033;--muted:#667085;--line:#d9e0ea;--blue:#1f6feb;--blue2:#dbeafe;--green:#12805c;--red:#c0352b;--amber:#b7791f;--shadow:0 14px 40px rgba(24,36,56,.09)}"
"body{margin:0;background:linear-gradient(180deg,#eef4ff 0,#f5f7fb 310px);color:var(--ink);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif}"
"main{max-width:1180px;margin:0 auto;padding:26px 18px 36px}"
"header{display:grid;grid-template-columns:1fr auto;gap:18px;align-items:end;margin-bottom:18px}"
"h1{font-size:clamp(1.8rem,4vw,3.1rem);line-height:1;margin:0;letter-spacing:0;font-weight:780}"
"h2{font-size:1rem;margin:0;font-weight:760}"
"p{margin:.4rem 0;color:var(--muted)}"
"a{color:var(--blue);text-decoration:none}a:hover{text-decoration:underline}"
".eyebrow{font-size:.74rem;font-weight:800;text-transform:uppercase;letter-spacing:.08em;color:#2e5aac;margin-bottom:.5rem}"
".subhead{max-width:650px;font-size:1rem}"
".toplinks{display:flex;gap:8px;flex-wrap:wrap;justify-content:flex-end}"
".linkbtn,.btn{appearance:none;border:1px solid var(--line);background:#fff;color:var(--ink);border-radius:8px;padding:.58rem .8rem;min-height:38px;font-weight:720;font-size:.9rem;cursor:pointer;box-shadow:none}"
".linkbtn{display:inline-flex;align-items:center}.btn:hover,.linkbtn:hover{border-color:#a9b7cc;background:#f8fbff;text-decoration:none}"
".btn.primary{background:var(--blue);border-color:var(--blue);color:#fff}.btn.primary:hover{background:#1558c8}"
".btn.good{background:var(--green);border-color:var(--green);color:#fff}.btn.danger{background:#fff1f0;border-color:#f0b8b2;color:#9f2018}.btn.ghost{background:transparent}"
".btn:disabled{opacity:.55;cursor:not-allowed}"
".stats{display:grid;grid-template-columns:repeat(5,minmax(0,1fr));gap:10px;margin:18px 0}"
".stat{background:rgba(255,255,255,.86);border:1px solid rgba(203,213,225,.8);border-radius:8px;padding:13px 14px;box-shadow:0 8px 24px rgba(24,36,56,.06)}"
".tabs{display:flex;gap:8px;flex-wrap:wrap;margin:16px 0}.tab{appearance:none;border:1px solid var(--line);background:#fff;color:var(--muted);border-radius:8px;padding:.68rem .95rem;min-height:40px;font-weight:800;cursor:pointer}.tab.active{background:var(--ink);border-color:var(--ink);color:#fff}.tabpane{display:none}.tabpane.active{display:block}.tabgrid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px;align-items:start}.tabgrid.single{grid-template-columns:1fr}"
".label{display:block;color:var(--muted);font-size:.72rem;font-weight:800;text-transform:uppercase;letter-spacing:.07em}"
".value{display:block;font-size:1.35rem;font-weight:790;margin-top:5px}.value.small{font-size:1rem}"
".grid{display:grid;grid-template-columns:minmax(0,1.15fr) minmax(330px,.85fr);gap:14px;align-items:start}"
".panel{background:var(--panel);border:1px solid var(--line);border-radius:8px;box-shadow:var(--shadow);overflow:hidden}"
".panel-head{display:flex;align-items:center;justify-content:space-between;gap:12px;padding:15px 16px;border-bottom:1px solid #e7ecf3;background:#fbfdff}"
".panel-body{padding:14px 16px}.stack{display:grid;gap:10px}"
".row{display:grid;grid-template-columns:1fr auto;gap:12px;align-items:center;border:1px solid #e6edf5;border-radius:8px;padding:12px;background:#fff}"
".row-title{display:flex;align-items:center;gap:9px;flex-wrap:wrap}.name{font-size:1rem;font-weight:780}"
".meta{display:flex;gap:8px;flex-wrap:wrap;margin-top:6px;color:var(--muted);font-size:.83rem}"
".desc{color:var(--muted);font-size:.86rem;line-height:1.35;margin-top:6px;max-width:52rem}"
".pill{display:inline-flex;align-items:center;gap:5px;border:1px solid #d9e3ef;background:#f8fafc;border-radius:999px;padding:.2rem .5rem;color:#475467;font-size:.78rem;font-weight:700}"
".dot{width:9px;height:9px;border-radius:999px;background:#98a2b3}.dot.ready{background:#12b76a}.dot.busy{background:#f59e0b}.dot.err{background:#e5483f}"
".actions{display:flex;gap:7px;flex-wrap:wrap;justify-content:flex-end}.split{display:flex;gap:8px;align-items:center;flex-wrap:wrap}"
"input,select,textarea{width:100%;border:1px solid #cfd8e5;border-radius:8px;padding:.55rem .65rem;min-height:38px;background:#fff;color:var(--ink);font:inherit}textarea{min-height:72px;resize:vertical}"
"input[type=number]{max-width:88px}input[type=range]{padding:0;min-height:24px;accent-color:var(--blue)}"
".rename{display:grid;grid-template-columns:minmax(140px,230px) auto;gap:7px;margin-top:9px}"
".empty{border:1px dashed #cbd5e1;border-radius:8px;padding:22px;text-align:center;color:var(--muted);background:#f8fafc}"
".scanbar{height:8px;border-radius:999px;background:#e7edf7;overflow:hidden;margin-top:10px}.scanbar span{display:block;height:100%;width:0;background:linear-gradient(90deg,#1f6feb,#14b8a6);transition:width .2s}"
".channels{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:12px}.channel{border:1px solid #e6edf5;border-radius:8px;padding:10px;background:#fbfdff}.channel b{display:block;font-size:.78rem;margin-bottom:8px}"
".settings{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.settings .wide{grid-column:1/-1}.check{display:flex;align-items:center;gap:9px;border:1px solid #e6edf5;border-radius:8px;padding:10px;background:#fbfdff}.check input{width:auto;min-height:auto}"
".profile-list{display:grid;gap:8px;margin-top:12px}.profile-bars{display:grid;grid-template-columns:repeat(9,1fr);gap:3px;margin-top:8px}.bar{height:28px;background:#dbeafe;border-radius:4px;position:relative;overflow:hidden}.bar span{position:absolute;left:0;right:0;bottom:0;background:#1f6feb}"
".toast{position:fixed;right:16px;bottom:16px;max-width:360px;background:#172033;color:#fff;padding:12px 14px;border-radius:8px;box-shadow:var(--shadow);font-weight:700;opacity:0;transform:translateY(8px);pointer-events:none;transition:.18s}.toast.show{opacity:1;transform:none}"
"@media(max-width:860px){header{grid-template-columns:1fr}.toplinks{justify-content:flex-start}.stats{grid-template-columns:repeat(2,minmax(0,1fr))}.grid,.tabgrid{grid-template-columns:1fr}.row{grid-template-columns:1fr}.actions{justify-content:flex-start}.channels{grid-template-columns:1fr 1fr}}"
"@media(max-width:520px){main{padding:18px 12px 28px}.stats{grid-template-columns:1fr}.panel-head{align-items:flex-start;flex-direction:column}.channels,.settings{grid-template-columns:1fr}.rename{grid-template-columns:1fr}.btn,.linkbtn{width:100%;justify-content:center}.actions,.split{width:100%}}"
"</style></head><body>"
"<main>"
"<header><div><div class=eyebrow>Open-source reef gateway</div><h1>HydraBridge ESP32</h1><p class=subhead>Local control for AquaIllumination lights and experimental AI pump support over BLE.</p></div><nav class=toplinks><a class=linkbtn href=/api/logs>Logs</a><a class=linkbtn href=/api/status>Status JSON</a></nav></header>"
"<section class=stats><div class=stat><span class=label>Controller</span><span id=ready class=value>--</span></div><div class=stat><span class=label>BLE State</span><span id=bleState class=\"value small\">--</span></div><div class=stat><span class=label>Lights</span><span id=lightCount class=value>0</span></div><div class=stat><span class=label>Pumps</span><span id=pumpCount class=value>0</span></div><div class=stat><span class=label>Uptime</span><span id=uptime class=\"value small\">--</span></div></section>"
"<nav class=tabs><button class=\"tab active\" data-tab=lights>Lights</button><button class=tab data-tab=profiles>Light Profiles</button><button class=tab data-tab=schedules>Lighting Schedules</button><button class=tab data-tab=pumps>Pumps</button><button class=tab data-tab=pumpschedules>Pump Schedules</button><button class=tab data-tab=settings>Settings</button></nav>"
"<section id=tab-lights class=\"tabpane active\"><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Registered Lights</h2><p>Operate paired lights and update names.</p></div><button id=refreshLights class=\"btn ghost\">Refresh</button></div><div id=lights class=\"panel-body stack\"><div class=empty>Loading lights&hellip;</div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Discovery</h2><p>Scan for nearby Mobius devices and add new lights or pumps.</p></div><button id=scanBtn class=\"btn primary\">Scan and Auto-Add</button></div><div class=panel-body><div id=scanmsg class=meta>Ready to scan for 30 seconds.</div><div class=scanbar><span id=scanProgress></span></div><div id=scanresults class=\"stack\" style=\"margin-top:12px\"></div></div></section>"
"</div><section class=panel style=\"margin-top:14px\"><div class=panel-head><div><h2>Light Groups</h2><p>Create groups for applying profiles to multiple lights together.</p></div><button id=refreshGroups class=\"btn ghost\">Refresh</button></div><div class=panel-body><div class=stack><label><span class=label>Group Name</span><input id=groupName maxlength=32 placeholder=\"Display tank\"></label><div id=groupMembers class=stack></div><div class=split><button id=saveGroup class=\"btn primary\">Save Group</button></div><div id=groups class=stack></div></div></div></section></section>"
"<section id=tab-profiles class=tabpane><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Profile Builder</h2><p>Save channel mixes and apply them to a selected light or group.</p></div></div><div class=panel-body><div class=stack><label><span class=label>Target</span><select id=targetSelect></select></label><label><span class=label>Profile Name</span><input id=profname maxlength=16 value=reef-day></label><label><span class=label>Description</span><textarea id=profdesc maxlength=128 placeholder=\"What this profile is for\"></textarea></label><div class=split><button id=saveProfile class=\"btn primary\">Save Profile</button><button id=applyCurrent class=btn>Apply Profile</button><button id=intUp class=btn>+ Intensity</button><button id=intDown class=btn>- Intensity</button></div></div><div id=channels class=channels></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Saved Profiles</h2><p>Apply, edit, or delete stored mixes.</p></div></div><div class=panel-body><div id=profiles class=profile-list><div class=empty>Loading profiles&hellip;</div></div></div></section>"
"</div></section>"
"<section id=tab-schedules class=tabpane><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Schedule Builder</h2><p>Automate profiles for a light or group.</p></div></div><div class=panel-body><div class=settings><input id=schedId type=hidden><label class=\"check wide\"><input id=schedEnabled type=checkbox checked><span><b>Enabled</b><br><span class=meta>Run this schedule when time is valid.</span></span></label><label><span class=label>Name</span><input id=schedName maxlength=32 placeholder=\"Reef day\"></label><label><span class=label>Target</span><select id=schedTarget></select></label><label><span class=label>Profile</span><select id=schedProfile></select></label><label><span class=label>Day Intensity %</span><input id=schedIntensity type=number min=0 max=100 value=70></label><label><span class=label>End Intensity %</span><input id=schedEndIntensity type=number min=0 max=100 value=0></label><label><span class=label>Start Trigger</span><select id=schedStartTrig><option value=0>Fixed time</option><option value=1>Sunrise</option><option value=2>Sunset</option></select></label><label><span class=label>End Trigger</span><select id=schedEndTrig><option value=0>Fixed time</option><option value=1>Sunrise</option><option value=2>Sunset</option></select></label><label><span class=label>Start Time</span><input id=schedStartTime type=time value=08:00></label><label><span class=label>End Time</span><input id=schedEndTime type=time value=18:00></label><label><span class=label>Start Offset Min</span><input id=schedStartOffset type=number min=-720 max=720 value=0></label><label><span class=label>End Offset Min</span><input id=schedEndOffset type=number min=-720 max=720 value=0></label><label><span class=label>Ramp Up Min</span><input id=schedRampUp type=number min=0 max=1440 value=90></label><label><span class=label>Ramp Down Min</span><input id=schedRampDown type=number min=0 max=1440 value=90></label><div class=\"split wide\"><button id=saveSchedule class=\"btn primary\">Save Schedule</button><button id=newSchedule class=btn>New</button></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Saved Schedules</h2><p id=scheduleNext>Next action loading.</p></div><button id=refreshSchedules class=\"btn ghost\">Refresh</button></div><div class=panel-body><div id=schedules class=stack><div class=empty>Loading schedules&hellip;</div></div></div></section>"
"</div></section>"
"<section id=tab-pumps class=tabpane><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Registered Pumps</h2><p>Experimental Orbit-style local controls.</p></div><button id=refreshPumps class=\"btn ghost\">Refresh</button></div><div id=pumps class=\"panel-body stack\"><div class=empty>Loading pumps&hellip;</div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Manual Pump Control</h2><p>Send an experimental live-demo pump scene.</p></div><span class=pill>Experimental</span></div><div class=panel-body><div class=settings><label class=wide><span class=label>Pump</span><select id=pumpTarget></select></label><label><span class=label>Mode</span><select id=pumpMode><option value=1>Constant Speed</option><option value=15>Random</option><option value=16>Pulse</option><option value=13>Feed</option></select></label><label><span class=label>Max Speed %</span><input id=pumpSpeed type=number min=0 max=100 value=40></label><label><span class=label>Min Speed %</span><input id=pumpMinSpeed type=number min=0 max=100 value=20></label><label><span class=label>Variance %</span><input id=pumpVariance type=number min=0 max=100 value=50></label><label><span class=label>On Time ms</span><input id=pumpOnMs type=number min=100 max=60000 value=1000></label><label><span class=label>Off Time ms</span><input id=pumpOffMs type=number min=100 max=60000 value=1000></label><div class=\"split wide\"><button id=applyPump class=\"btn primary\">Apply</button><button id=feedPump class=btn>Feed</button><button id=offPump class=\"btn danger\">Off</button></div></div></div></section>"
"</div></section>"
"<section id=tab-pumpschedules class=tabpane><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Pump Schedule Builder</h2><p>Run one pump command at start and another at end.</p></div></div><div class=panel-body><div class=settings><input id=pumpSchedId type=hidden><label class=\"check wide\"><input id=pumpSchedEnabled type=checkbox checked><span><b>Enabled</b><br><span class=meta>Run this pump schedule when time is valid.</span></span></label><label><span class=label>Name</span><input id=pumpSchedName maxlength=32 placeholder=\"Reef flow\"></label><label><span class=label>Pump</span><select id=pumpSchedTarget></select></label><label><span class=label>Active Mode</span><select id=pumpSchedActiveMode><option value=1>Constant Speed</option><option value=15>Random</option><option value=16>Pulse</option><option value=13>Feed</option></select></label><label><span class=label>Active Max %</span><input id=pumpSchedActiveSpeed type=number min=0 max=100 value=40></label><label><span class=label>Active Min %</span><input id=pumpSchedActiveMin type=number min=0 max=100 value=20></label><label><span class=label>Active Variance %</span><input id=pumpSchedActiveVariance type=number min=0 max=100 value=50></label><label><span class=label>Active On ms</span><input id=pumpSchedActiveOn type=number min=100 max=60000 value=1000></label><label><span class=label>Active Off ms</span><input id=pumpSchedActiveOff type=number min=100 max=60000 value=1000></label><label><span class=label>End Mode</span><select id=pumpSchedEndMode><option value=1>Constant Speed</option><option value=15>Random</option><option value=16>Pulse</option><option value=13>Feed</option></select></label><label><span class=label>End Max %</span><input id=pumpSchedEndSpeed type=number min=0 max=100 value=20></label><label><span class=label>End Min %</span><input id=pumpSchedEndMin type=number min=0 max=100 value=10></label><label><span class=label>End Variance %</span><input id=pumpSchedEndVariance type=number min=0 max=100 value=50></label><label><span class=label>End On ms</span><input id=pumpSchedEndOn type=number min=100 max=60000 value=1000></label><label><span class=label>End Off ms</span><input id=pumpSchedEndOff type=number min=100 max=60000 value=1000></label><label><span class=label>Start Trigger</span><select id=pumpSchedStartTrig><option value=0>Fixed time</option><option value=1>Sunrise</option><option value=2>Sunset</option></select></label><label><span class=label>End Trigger</span><select id=pumpSchedEndTrig><option value=0>Fixed time</option><option value=1>Sunrise</option><option value=2>Sunset</option></select></label><label><span class=label>Start Time</span><input id=pumpSchedStartTime type=time value=08:00></label><label><span class=label>End Time</span><input id=pumpSchedEndTime type=time value=18:00></label><label><span class=label>Start Offset Min</span><input id=pumpSchedStartOffset type=number min=-720 max=720 value=0></label><label><span class=label>End Offset Min</span><input id=pumpSchedEndOffset type=number min=-720 max=720 value=0></label><div class=\"split wide\"><button id=savePumpSchedule class=\"btn primary\">Save Pump Schedule</button><button id=newPumpSchedule class=btn>New</button></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Saved Pump Schedules</h2><p id=pumpScheduleNext>Next pump action loading.</p></div><button id=refreshPumpSchedules class=\"btn ghost\">Refresh</button></div><div class=panel-body><div id=pumpSchedules class=stack><div class=empty>Loading pump schedules&hellip;</div></div></div></section>"
"</div></section>"
"<section id=tab-settings class=tabpane><div class=tabgrid>"
"<section class=panel><div class=panel-head><div><h2>Time Sync</h2><p>Keep local time for schedules and logs.</p></div><span id=timeState class=pill>Loading</span></div><div class=panel-body><div class=settings><label class=\"check wide\"><input id=timeEnabled type=checkbox><span><b>Enable NTP time sync</b><br><span class=meta>Uses SNTP and does not block local control.</span></span></label><label><span class=label>Time Server</span><input id=timeServer maxlength=64></label><label><span class=label>Timezone</span><select id=timeTz><option value=UTC0>UTC</option><option value=\"EST5EDT,M3.2.0/2,M11.1.0/2\">Eastern</option><option value=\"CST6CDT,M3.2.0/2,M11.1.0/2\">Central</option><option value=\"MST7MDT,M3.2.0/2,M11.1.0/2\">Mountain</option><option value=\"PST8PDT,M3.2.0/2,M11.1.0/2\">Pacific</option><option value=MST7>Arizona</option><option value=HST10>Hawaii</option></select></label><div class=\"split wide\"><button id=saveTime class=\"btn primary\">Save Time</button><span id=timeNow class=meta>--</span></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>Sun Events</h2><p>Compute sunrise and sunset locally from coordinates.</p></div><span id=sunState class=pill>Loading</span></div><div class=panel-body><div class=settings><label class=\"check wide\"><input id=sunEnabled type=checkbox><span><b>Enable sunrise/sunset triggers</b><br><span class=meta>No location data is sent to an external API.</span></span></label><label><span class=label>Location Label</span><input id=sunLabel maxlength=32></label><label><span class=label>Latitude</span><input id=sunLat type=number step=0.000001 min=-90 max=90></label><label><span class=label>Longitude</span><input id=sunLon type=number step=0.000001 min=-180 max=180></label><div class=\"split wide\"><button id=saveSun class=\"btn primary\">Save Sun Events</button><span id=sunTimes class=meta>--</span></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>WiFi</h2><p>Connect to your home network or use setup hotspot fallback.</p></div><span id=wifiState class=pill>Loading</span></div><div class=panel-body><div class=settings><label class=\"check wide\"><input id=wifiEnabled type=checkbox><span><b>Enable WiFi station</b><br><span class=meta>Connects to the configured home WiFi.</span></span></label><label><span class=label>SSID</span><input id=wifiSsid maxlength=32 autocomplete=off></label><label><span class=label>Password</span><input id=wifiPass maxlength=64 type=password autocomplete=current-password></label><label class=\"check wide\"><input id=wifiApFallback type=checkbox><span><b>Setup hotspot fallback</b><br><span class=meta>Starts setup AP if no WiFi is configured or connection fails.</span></span></label><label><span class=label>Setup AP Name</span><input id=wifiApSsid maxlength=32></label><label><span class=label>Setup AP Password</span><input id=wifiApPass maxlength=64 type=password></label><div class=\"split wide\"><button id=saveWifi class=\"btn primary\">Save WiFi</button><span class=meta>Setup URL: http://192.168.1.10/</span></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>RS485 Slave</h2><p>Allow another controller to command registered lights over Modbus RTU.</p></div><span id=rs485State class=pill>Loading</span></div><div class=panel-body><div class=settings><label class=\"check wide\"><input id=mbEnabled type=checkbox><span><b>Enable RS485 slave</b><br><span class=meta>Off by default. Saves to NVS.</span></span></label><label><span class=label>Slave Address</span><input id=mbAddr type=number min=1 max=247></label><label><span class=label>Baud Rate</span><input id=mbBaud type=number min=1200 max=921600 step=100></label><label><span class=label>Parity</span><select id=mbParity><option value=0>None</option><option value=1>Even</option><option value=2>Odd</option></select></label><label><span class=label>UART Port</span><input id=mbUart type=number min=0 max=2></label><label><span class=label>TX GPIO</span><input id=mbTx type=number min=-1 max=48></label><label><span class=label>RX GPIO</span><input id=mbRx type=number min=-1 max=48></label><label><span class=label>DE/RTS GPIO</span><input id=mbDe type=number min=-1 max=48></label><div class=\"split wide\"><button id=saveModbus class=\"btn primary\">Save RS485</button><span class=meta>Default wiring: RX 18, TX 17.</span></div></div></div></section>"
"<section class=panel><div class=panel-head><div><h2>MQTT</h2><p>Connect HydraBridge to a local MQTT broker.</p></div><span id=mqttState class=pill>Loading</span></div><div class=panel-body><div class=settings><label class=\"check wide\"><input id=mqEnabled type=checkbox><span><b>Enable MQTT</b><br><span class=meta>Off by default. Saves to NVS.</span></span></label><label><span class=label>Broker Host</span><input id=mqHost maxlength=64 placeholder=\"192.168.1.10\"></label><label><span class=label>Port</span><input id=mqPort type=number min=1 max=65535></label><label><span class=label>Username</span><input id=mqUser maxlength=32 autocomplete=username></label><label><span class=label>Password</span><input id=mqPass maxlength=64 type=password autocomplete=current-password></label><label><span class=label>Client ID</span><input id=mqClient maxlength=32></label><label><span class=label>Keepalive Sec</span><input id=mqKeepalive type=number min=15 max=3600></label><label><span class=label>Base Topic</span><input id=mqBase maxlength=32></label><label><span class=label>HA Discovery Prefix</span><input id=mqHaPrefix maxlength=32></label><label class=\"check wide\"><input id=mqTls type=checkbox><span><b>Use TLS</b><br><span class=meta>Uses mqtts on the selected port.</span></span></label><label class=\"check wide\"><input id=mqHa type=checkbox><span><b>Home Assistant friendly</b><br><span class=meta>Publish MQTT discovery entities when connected.</span></span></label><div class=\"split wide\"><button id=saveMqtt class=\"btn primary\">Save MQTT</button><span class=meta>Commands use base/controller/light/&lt;id&gt;/set and group/&lt;id&gt;/set.</span></div></div></div></section>"
"</div></section><div id=toast class=toast></div></main>"
"<script>"
"const $=id=>document.getElementById(id);"
"let appLights=[];let appGroups=[];let appProfiles=[];let appSchedules=[];let appPumps=[];let appPumpSchedules=[];"
"const channelNames=['brightness','coolwhite','blue','deepred','violet','uv','green','royalblue','moonlight'];"
"const channelLabels=['Brightness','Cool White','Blue','Deep Red','Violet','UV','Green','Royal Blue','Moonlight'];"
"const stateNames={0:'Disabled',1:'Idle',3:'Connecting',4:'Discovering',5:'Subscribing',7:'Ready',8:'Writing',9:'Waiting Confirm',10:'Cooldown',11:'Disconnecting',12:'Error',13:'Backoff'};"
"function modelLabel(m){const t={320:'Prime HD',321:'Prime Freshwater',322:'Prime 16HD',335:'Hydra 64HD',336:'Hydra 32HD',337:'Hydra 26HD',338:'Blade'};return t[m]||('AI Light '+m);}"
"function el(tag,cls,txt){const n=document.createElement(tag);if(cls)n.className=cls;if(txt!==undefined)n.textContent=txt;return n;}"
"function toast(msg){const t=$('toast');t.textContent=msg;t.classList.add('show');clearTimeout(window.toastTimer);window.toastTimer=setTimeout(()=>t.classList.remove('show'),2600);}"
"async function fetchJson(u,opt){const r=await fetch(u,opt);if(!r.ok)throw new Error(await r.text()||r.statusText);return r.json();}"
"const jget=u=>fetchJson(u);"
"const jpost=(u,b)=>fetchJson(u,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):''});"
"const jdel=u=>fetchJson(u,{method:'DELETE'});"
"function fmtUptime(ms){let s=Math.floor((ms||0)/1000),h=Math.floor(s/3600),m=Math.floor((s%3600)/60);return h?`${h}h ${m}m`:`${m}m ${s%60}s`;}"
"function bleDot(st){if(st===7)return'ready';if(st===12||st===13)return'err';if(st&&st!==1)return'busy';return'';}"
"async function refreshStatus(){try{const s=await jget('/api/status');$('ready').textContent=s.controller_ready?'Ready':'Offline';$('lightCount').textContent=s.registered_lights||0;$('pumpCount').textContent=s.registered_pumps||0;$('uptime').textContent=fmtUptime(s.uptime_ms);$('bleState').textContent=stateNames[s.ble_state]||String(s.ble_state);if($('scheduleNext'))$('scheduleNext').textContent=s.next_schedule_action||'No scheduled action';if($('pumpScheduleNext'))$('pumpScheduleNext').textContent=s.next_pump_schedule_action||'No pump action';}catch(e){$('ready').textContent='Error';$('bleState').textContent='Status failed';}}"
"function addMeta(parent,items){const m=el('div','meta');for(const it of items){const p=el('span','pill',it);m.appendChild(p);}parent.appendChild(m);}"
"function fillTargetSelect(sel){if(!sel)return;const old=sel.value;sel.innerHTML='';for(const l of appLights){const o=document.createElement('option');o.value='light:'+l.light_id;o.textContent='Light: '+(l.display_name||l.light_id);sel.appendChild(o);}for(const g of appGroups){const o=document.createElement('option');o.value='group:'+g.group_id;o.textContent='Group: '+(g.display_name||g.group_id);sel.appendChild(o);}if(old)sel.value=old;if(!sel.value&&sel.options.length)sel.selectedIndex=0;}"
"function refreshTargetSelect(){fillTargetSelect($('targetSelect'));fillTargetSelect($('schedTarget'));}"
"function refreshScheduleProfileOptions(){const sel=$('schedProfile');if(!sel)return;const old=sel.value;sel.innerHTML='';for(const p of appProfiles){const o=document.createElement('option');o.value=p.name;o.textContent=p.name;sel.appendChild(o);}if(old)sel.value=old;if(!sel.value&&sel.options.length)sel.selectedIndex=0;}"
"function refreshPumpTargetSelects(){['pumpTarget','pumpSchedTarget'].forEach(id=>{const sel=$(id);if(!sel)return;const old=sel.value;sel.innerHTML='';for(const p of appPumps){const o=document.createElement('option');o.value=p.pump_id;o.textContent=p.display_name||p.pump_id;sel.appendChild(o);}if(old)sel.value=old;if(!sel.value&&sel.options.length)sel.selectedIndex=0;});}"
"function renderGroupMembers(){const box=$('groupMembers');if(!box)return;box.innerHTML='';if(!appLights.length){box.appendChild(el('div','empty','Add lights before creating a group.'));return;}for(const l of appLights){const lab=el('label','check');const cb=document.createElement('input');cb.type='checkbox';cb.value=l.light_id;cb.className='groupMember';lab.appendChild(cb);lab.appendChild(el('span',null,l.display_name||l.light_id));box.appendChild(lab);}}"
"async function refreshLights(){try{const r=await jget('/api/lights');appLights=r.lights||[];const box=$('lights');box.innerHTML='';if(!appLights.length){box.appendChild(el('div','empty','No registered lights yet. Start a discovery scan to add one.'));}else{for(const l of appLights)box.appendChild(lightRow(l));}renderGroupMembers();refreshTargetSelect();}catch(e){toast('Lights failed: '+e.message);}}"
"function lightRow(l){const row=el('article','row');const left=el('div');const title=el('div','row-title');const dot=el('span','dot');title.appendChild(dot);title.appendChild(el('span','name',l.display_name||l.light_id));title.appendChild(el('span','pill',l.enabled?'Enabled':'Disabled'));left.appendChild(title);addMeta(left,[modelLabel(l.model),l.serial||'No serial','RSSI '+l.last_seen_rssi,l.light_id]);const ren=el('div','rename');const inp=document.createElement('input');inp.value=l.display_name||'';inp.maxLength=32;inp.setAttribute('aria-label','Light name');const save=el('button','btn','Save Name');save.onclick=()=>renameLight(l.light_id,inp.value);ren.appendChild(inp);ren.appendChild(save);left.appendChild(ren);const act=el('div','actions');[['On',()=>cmd(l.light_id,'on'),'good'],['Off',()=>cmd(l.light_id,'off'),'danger'],['+ Int',()=>intensity(l.light_id,50),''],['- Int',()=>intensity(l.light_id,-50),''],['Remove',()=>removeLight(l.light_id),'ghost']].forEach(a=>{const b=el('button','btn '+a[2],a[0]);b.onclick=a[1];act.appendChild(b);});row.appendChild(left);row.appendChild(act);return row;}"
"async function renameLight(id,name){const clean=(name||'').trim();if(!clean)return toast('Name is required');if(/[\"\\\\<>]/.test(clean))return toast('Name cannot contain quotes, backslashes, <, or >');try{await jpost('/api/lights/'+id+'/rename',{display_name:clean});toast('Light renamed');refreshLights();}catch(e){toast('Rename failed: '+e.message);}}"
"async function removeLight(id){if(!confirm('Remove '+id+'?'))return;try{await jdel('/api/lights/'+id);toast('Light removed');refreshLights();}catch(e){toast('Remove failed: '+e.message);}}"
"async function refreshGroups(){try{const r=await jget('/api/groups');appGroups=r.groups||[];const box=$('groups');box.innerHTML='';if(!appGroups.length){box.appendChild(el('div','empty','No groups yet. Select lights above and save a group.'));}else{for(const g of appGroups)box.appendChild(groupRow(g));}refreshTargetSelect();}catch(e){toast('Groups failed: '+e.message);}}"
"function groupRow(g){const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',g.display_name||g.group_id));title.appendChild(el('span','pill',(g.member_count||0)+' lights'));left.appendChild(title);addMeta(left,(g.members||[]).map(m=>'Light '+m));const act=el('div','actions');const del=el('button','btn danger','Delete');del.onclick=()=>deleteGroup(g.group_id);act.appendChild(del);row.appendChild(left);row.appendChild(act);return row;}"
"async function saveGroup(){const name=($('groupName').value||'').trim();if(!name)return toast('Group name is required');if(/[\"\\\\<>]/.test(name))return toast('Group name cannot contain quotes, backslashes, <, or >');const members=[...document.querySelectorAll('.groupMember:checked')].map(c=>c.value);if(!members.length)return toast('Select at least one light');const gid='grp-'+name.replace(/[^a-zA-Z0-9]/g,'').slice(0,20);try{await jpost('/api/groups',{group_id:gid,display_name:name,members:members.join(',')});toast('Group saved');$('groupName').value='';document.querySelectorAll('.groupMember').forEach(c=>c.checked=false);refreshGroups();}catch(e){toast('Group save failed: '+e.message);}}"
"async function deleteGroup(id){if(!confirm('Delete group '+id+'?'))return;try{await jdel('/api/groups/'+id);toast('Group deleted');refreshGroups();}catch(e){toast('Delete group failed: '+e.message);}}"
"async function startScan(){const btn=$('scanBtn'),msg=$('scanmsg'),bar=$('scanProgress');btn.disabled=true;bar.style.width='0';try{const d=await jpost('/api/scan');const dur=(d&&d.duration_ms)||30000;const start=Date.now();msg.textContent='Scanning for Hydra lights and Orbit pumps...';clearInterval(window.scanTimer);window.scanTimer=setInterval(()=>{const pct=Math.min(100,((Date.now()-start)/dur)*100);bar.style.width=pct+'%';},250);setTimeout(refreshScanResults,dur+1200);}catch(e){btn.disabled=false;msg.textContent='Scan failed';toast('Scan failed: '+e.message);}}"
"async function refreshScanResults(){clearInterval(window.scanTimer);$('scanProgress').style.width='100%';$('scanBtn').disabled=false;try{const r=await jget('/api/scan/results');const box=$('scanresults');box.innerHTML='';const list=r.results||[];$('scanmsg').textContent=list.length?`${list.length} device${list.length===1?'':'s'} found.`:'No devices found. Put the device in pairing mode and scan again.';for(const l of list)box.appendChild(scanRow(l));refreshLights();refreshPumps();}catch(e){toast('Scan results failed: '+e.message);}}"
"function scanRow(l){let serial=l.serial&&l.serial.length>0&&l.model!=0?l.serial:l.ble_addr.replace(/:/g,'').toUpperCase().slice(0,10);const clean=serial.replace(/[^a-zA-Z0-9]/g,'').slice(0,10);const isPump=l.device_type==='pump';const lid='hydra64-'+clean,pid='pump-'+clean;let label=isPump?'AI Pump':(l.model==0?'Unknown pairing device':modelLabel(l.model));const shown=l.name||label,useModel=l.model||335,useSerial=l.serial&&l.serial.length>0?l.serial:l.ble_addr;const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',shown));title.appendChild(el('span','pill',label));left.appendChild(title);addMeta(left,[useSerial,l.ble_addr+' t='+l.ble_addr_type,'RSSI '+l.rssi]);const act=el('div','actions');const b=el('button','btn primary',isPump?'Add Pump':'Add Light');b.onclick=()=>isPump?addPump(pid,shown,l.ble_addr,l.ble_addr_type,useSerial,useModel):addLight(lid,shown,l.ble_addr,l.ble_addr_type,useSerial,useModel);act.appendChild(b);if(!isPump){const pb=el('button','btn','Add as Pump');pb.onclick=()=>addPump(pid,shown,l.ble_addr,l.ble_addr_type,useSerial,useModel);act.appendChild(pb);}row.appendChild(left);row.appendChild(act);return row;}"
"async function addLight(id,name,addr,addr_type,serial,model){try{const r=await jpost('/api/lights',{light_id:id,display_name:name,ble_addr:addr,ble_addr_type:addr_type,serial:serial,model:model});toast(r.added?'Light added':'Light already registered');refreshLights();}catch(e){toast('Add failed: '+e.message);}}"
"async function addPump(id,name,addr,addr_type,serial,model){try{const r=await jpost('/api/pumps',{pump_id:id,display_name:name,ble_addr:addr,ble_addr_type:addr_type,serial:serial,model:model});toast(r.added?'Pump added':'Pump already registered');refreshPumps();}catch(e){toast('Add pump failed: '+e.message);}}"
"async function refreshPumps(){try{const r=await jget('/api/pumps');appPumps=r.pumps||[];const box=$('pumps');if(!box)return;box.innerHTML='';if(!appPumps.length){box.appendChild(el('div','empty','No registered pumps yet. Scan to add an AI pump.'));}else{for(const p of appPumps)box.appendChild(pumpRow(p));}refreshPumpTargetSelects();refreshStatus();}catch(e){toast('Pumps failed: '+e.message);}}"
"function pumpRow(p){const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',p.display_name||p.pump_id));title.appendChild(el('span','pill',p.enabled?'Enabled':'Disabled'));title.appendChild(el('span','pill','Experimental'));left.appendChild(title);addMeta(left,[p.serial||p.pump_id,'model '+(p.model||0),'RSSI '+p.last_seen_rssi,'mode '+pumpModeLabel(p.last_mode||0),'speed '+(p.last_speed_percent||0)+'%']);const ren=el('div','rename');const inp=document.createElement('input');inp.value=p.display_name||'';inp.maxLength=32;const rb=el('button','btn','Rename');rb.onclick=()=>renamePump(p.pump_id,inp.value);ren.appendChild(inp);ren.appendChild(rb);left.appendChild(ren);const act=el('div','actions');[['Apply',()=>pumpCommand(p.pump_id,pumpCommandBody()),'primary'],['Random',()=>pumpCommand(p.pump_id,{mode:15,speed_percent:60,min_speed_percent:20,variance_percent:50,timeout:60}),''],['Pulse',()=>pumpCommand(p.pump_id,{mode:16,speed_percent:60,on_time_ms:1000,off_time_ms:1000,timeout:60}),''],['Feed',()=>pumpCommand(p.pump_id,{mode:13,speed_percent:10,timeout:60}),''],['Off',()=>pumpCommand(p.pump_id,{mode:1,speed_percent:0,timeout:60}),'danger'],['Remove',()=>removePump(p.pump_id),'danger']].forEach(a=>{const b=el('button','btn '+a[2],a[0]);b.onclick=a[1];act.appendChild(b);});row.appendChild(left);row.appendChild(act);return row;}"
"async function renamePump(id,name){const clean=(name||'').trim();if(!clean)return toast('Name is required');if(/[\"\\\\<>]/.test(clean))return toast('Name cannot contain quotes, backslashes, <, or >');try{await jpost('/api/pumps/'+id+'/rename',{display_name:clean});toast('Pump renamed');refreshPumps();}catch(e){toast('Pump rename failed: '+e.message);}}"
"async function removePump(id){if(!confirm('Remove '+id+'?'))return;try{await jdel('/api/pumps/'+id);toast('Pump removed');refreshPumps();}catch(e){toast('Remove pump failed: '+e.message);}}"
"function pumpCommandBody(){return{mode:parseInt($('pumpMode').value)||1,speed_percent:parseInt($('pumpSpeed').value)||0,min_speed_percent:parseInt($('pumpMinSpeed').value)||0,variance_percent:parseInt($('pumpVariance').value)||0,on_time_ms:parseInt($('pumpOnMs').value)||1000,off_time_ms:parseInt($('pumpOffMs').value)||1000,timeout:60};}"
"async function pumpCommand(id,body){if(!id)return toast('Select a pump first');try{await jpost('/api/pumps/'+id+'/command',body);toast('Pump command queued');refreshPumps();}catch(e){toast('Pump command failed: '+e.message);}}"
"async function cmd(id,power){try{await jpost('/api/lights/'+id+'/command',{power:power,timeout:60});toast(power==='on'?'Power on queued':'Power off queued');}catch(e){toast('Command failed: '+e.message);}}"
"async function intensity(id,delta){try{await jpost('/api/lights/'+id+'/command',{intensity_delta:delta,timeout:60});toast('Intensity command queued');}catch(e){toast('Intensity failed: '+e.message);}}"
"function currentTarget(){const v=$('targetSelect').value||'';const p=v.split(':');return{type:p[0]||'',id:p.slice(1).join(':')};}"
"function targetUrl(t){return t.type==='group'?'/api/groups/'+t.id+'/command':'/api/lights/'+t.id+'/command';}"
"function intensityDelta(d){const t=currentTarget();if(t.id)intensityTarget(t,d);else toast('Select a target first');}"
"function buildChannels(){const box=$('channels');box.innerHTML='';channelNames.forEach((n,i)=>{const c=el('label','channel');c.appendChild(el('b',null,channelLabels[i]));const range=document.createElement('input');range.type='range';range.min=0;range.max=1000;range.value=i===0?1000:0;range.id='ch'+i;const num=document.createElement('input');num.type='number';num.min=0;num.max=1000;num.value=range.value;range.oninput=()=>num.value=range.value;num.oninput=()=>range.value=Math.max(0,Math.min(1000,parseInt(num.value)||0));c.appendChild(range);c.appendChild(num);box.appendChild(c);});}"
"function profileBody(name){const desc=($('profdesc').value||'').trim();const b={name:name,description:desc};for(let j=0;j<9;j++)b[channelNames[j]]=parseInt($('ch'+j).value)||0;return b;}"
"async function saveProfile(){const name=($('profname').value||'profile').trim();const desc=($('profdesc').value||'').trim();if(!name)return toast('Profile name is required');if(/[\"\\\\<>]/.test(name+desc))return toast('Profile text cannot contain quotes, backslashes, <, or >');try{await jpost('/api/profiles',profileBody(name));toast('Profile saved');refreshProfiles();}catch(e){toast('Save failed: '+e.message);}}"
"async function refreshProfiles(){try{const r=await jget('/api/profiles');const box=$('profiles');box.innerHTML='';const ps=r.profiles||[];appProfiles=ps;refreshScheduleProfileOptions();if(!ps.length){box.appendChild(el('div','empty','No profiles saved yet.'));return;}for(const pr of ps)box.appendChild(profileRow(pr));}catch(e){toast('Profiles failed: '+e.message);}}"
"function profileRow(pr){const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',pr.name));if(pr.builtin)title.appendChild(el('span','pill','Built-in'));left.appendChild(title);if(pr.description)left.appendChild(el('div','desc',pr.description));const bars=el('div','profile-bars');(pr.intensities||[]).forEach(v=>{const b=el('div','bar');const s=document.createElement('span');s.style.height=Math.max(3,Math.min(100,Math.round((v||0)/10)))+'%';b.appendChild(s);bars.appendChild(b);});left.appendChild(bars);const act=el('div','actions');let actions=[['Apply',()=>applyProfile(pr.name),'primary'],['Load',()=>loadProfile(pr),'']];if(!pr.builtin)actions.push(['Delete',()=>delProfile(pr.name),'danger']);actions.forEach(a=>{const b=el('button','btn '+a[2],a[0]);b.onclick=a[1];act.appendChild(b);});row.appendChild(left);row.appendChild(act);return row;}"
"function loadProfile(pr){$('profname').value=pr.name;$('profdesc').value=pr.description||'';(pr.intensities||[]).forEach((v,i)=>{const r=$('ch'+i);if(r){r.value=v;const n=r.parentNode.querySelector('input[type=number]');if(n)n.value=v;}});toast('Profile loaded');}"
"async function applyProfile(name){const t=currentTarget();if(!t.id)return toast('Select a target first');try{await jpost(targetUrl(t),{profile:name,timeout:60});toast(t.type==='group'?'Group profile queued':'Profile queued');}catch(e){toast('Apply failed: '+e.message);}}"
"async function intensityTarget(t,delta){try{await jpost(targetUrl(t),{intensity_delta:delta,timeout:60});toast('Intensity command queued');}catch(e){toast('Intensity failed: '+e.message);}}"
"async function delProfile(name){if(!confirm('Delete '+name+'?'))return;try{await jdel('/api/profiles/'+encodeURIComponent(name));toast('Profile deleted');refreshProfiles();}catch(e){toast('Delete failed: '+e.message);}}"
"function minToTime(m){m=parseInt(m)||0;return String(Math.floor(m/60)).padStart(2,'0')+':'+String(m%60).padStart(2,'0');}"
"function timeToMin(v){const p=(v||'00:00').split(':');return (parseInt(p[0])||0)*60+(parseInt(p[1])||0);}"
"function parseTargetValue(v){const p=(v||'').split(':');return{type:p[0]==='group'?1:0,id:p.slice(1).join(':')};}"
"function clearScheduleForm(){$('schedId').value='';$('schedEnabled').checked=true;$('schedName').value='';$('schedIntensity').value=70;$('schedEndIntensity').value=0;$('schedStartTrig').value=0;$('schedEndTrig').value=0;$('schedStartTime').value='08:00';$('schedEndTime').value='18:00';$('schedStartOffset').value=0;$('schedEndOffset').value=0;$('schedRampUp').value=90;$('schedRampDown').value=90;refreshTargetSelect();refreshScheduleProfileOptions();}"
"function scheduleBody(){const t=parseTargetValue($('schedTarget').value);return{schedule_id:$('schedId').value||'',enabled:$('schedEnabled').checked,name:($('schedName').value||'Schedule').trim(),target_type:t.type,target_id:t.id,profile_name:$('schedProfile').value||'',intensity_percent:parseInt($('schedIntensity').value)||0,end_intensity_percent:parseInt($('schedEndIntensity').value)||0,start_trigger:parseInt($('schedStartTrig').value)||0,end_trigger:parseInt($('schedEndTrig').value)||0,start_minute:timeToMin($('schedStartTime').value),end_minute:timeToMin($('schedEndTime').value),start_offset_min:parseInt($('schedStartOffset').value)||0,end_offset_min:parseInt($('schedEndOffset').value)||0,ramp_up_min:parseInt($('schedRampUp').value)||0,ramp_down_min:parseInt($('schedRampDown').value)||0};}"
"async function saveSchedule(){const b=scheduleBody();if(!b.target_id)return toast('Select a schedule target');if(!b.profile_name)return toast('Select a profile');if(/[\"\\\\<>]/.test(b.name+b.target_id+b.profile_name))return toast('Schedule text cannot contain quotes, backslashes, <, or >');try{const r=await jpost('/api/schedules',b);$('schedId').value=r.schedule_id||'';toast('Schedule saved');refreshSchedules();}catch(e){toast('Schedule save failed: '+e.message);}}"
"async function refreshSchedules(){try{const r=await jget('/api/schedules');appSchedules=r.schedules||[];$('scheduleNext').textContent=r.next_action||'No scheduled action';const box=$('schedules');box.innerHTML='';if(!appSchedules.length){box.appendChild(el('div','empty','No schedules yet.'));return;}for(const s of appSchedules)box.appendChild(scheduleRow(s));}catch(e){toast('Schedules failed: '+e.message);}}"
"function trigLabel(v){return({0:'Fixed',1:'Sunrise',2:'Sunset'})[v]||String(v);}"
"function scheduleRow(s){const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',s.name||s.schedule_id));title.appendChild(el('span','pill',s.enabled?'Enabled':'Disabled'));left.appendChild(title);const target=(s.target_type===1?'Group ':'Light ')+(s.target_id||'');addMeta(left,[target,s.profile_name||'No profile',(s.intensity_percent||0)+'%','Start '+trigLabel(s.start_trigger)+' '+minToTime(s.start_minute||0),'End '+trigLabel(s.end_trigger)+' '+minToTime(s.end_minute||0)]);const act=el('div','actions');[['Edit',()=>loadSchedule(s),'primary'],['Delete',()=>deleteSchedule(s.schedule_id),'danger']].forEach(a=>{const b=el('button','btn '+a[2],a[0]);b.onclick=a[1];act.appendChild(b);});row.appendChild(left);row.appendChild(act);return row;}"
"function loadSchedule(s){$('schedId').value=s.schedule_id||'';$('schedEnabled').checked=!!s.enabled;$('schedName').value=s.name||'';$('schedTarget').value=(s.target_type===1?'group:':'light:')+(s.target_id||'');$('schedProfile').value=s.profile_name||'';$('schedIntensity').value=s.intensity_percent||0;$('schedEndIntensity').value=s.end_intensity_percent||0;$('schedStartTrig').value=s.start_trigger||0;$('schedEndTrig').value=s.end_trigger||0;$('schedStartTime').value=minToTime(s.start_minute||0);$('schedEndTime').value=minToTime(s.end_minute||0);$('schedStartOffset').value=s.start_offset_min||0;$('schedEndOffset').value=s.end_offset_min||0;$('schedRampUp').value=s.ramp_up_min||0;$('schedRampDown').value=s.ramp_down_min||0;document.querySelector('[data-tab=schedules]').click();}"
"async function deleteSchedule(id){if(!confirm('Delete schedule '+id+'?'))return;try{await jdel('/api/schedules/'+encodeURIComponent(id));toast('Schedule deleted');clearScheduleForm();refreshSchedules();}catch(e){toast('Delete schedule failed: '+e.message);}}"
"function clearPumpScheduleForm(){$('pumpSchedId').value='';$('pumpSchedEnabled').checked=true;$('pumpSchedName').value='';$('pumpSchedActiveMode').value=1;$('pumpSchedActiveSpeed').value=40;$('pumpSchedActiveMin').value=20;$('pumpSchedActiveVariance').value=50;$('pumpSchedActiveOn').value=1000;$('pumpSchedActiveOff').value=1000;$('pumpSchedEndMode').value=1;$('pumpSchedEndSpeed').value=20;$('pumpSchedEndMin').value=10;$('pumpSchedEndVariance').value=50;$('pumpSchedEndOn').value=1000;$('pumpSchedEndOff').value=1000;$('pumpSchedStartTrig').value=0;$('pumpSchedEndTrig').value=0;$('pumpSchedStartTime').value='08:00';$('pumpSchedEndTime').value='18:00';$('pumpSchedStartOffset').value=0;$('pumpSchedEndOffset').value=0;refreshPumpTargetSelects();}"
"function pumpScheduleBody(){return{schedule_id:$('pumpSchedId').value||'',enabled:$('pumpSchedEnabled').checked,name:($('pumpSchedName').value||'Pump Schedule').trim(),target_id:$('pumpSchedTarget').value||'',active_mode:parseInt($('pumpSchedActiveMode').value)||1,active_speed_percent:parseInt($('pumpSchedActiveSpeed').value)||0,active_min_speed_percent:parseInt($('pumpSchedActiveMin').value)||0,active_variance_percent:parseInt($('pumpSchedActiveVariance').value)||0,active_on_time_ms:parseInt($('pumpSchedActiveOn').value)||1000,active_off_time_ms:parseInt($('pumpSchedActiveOff').value)||1000,end_mode:parseInt($('pumpSchedEndMode').value)||1,end_speed_percent:parseInt($('pumpSchedEndSpeed').value)||0,end_min_speed_percent:parseInt($('pumpSchedEndMin').value)||0,end_variance_percent:parseInt($('pumpSchedEndVariance').value)||0,end_on_time_ms:parseInt($('pumpSchedEndOn').value)||1000,end_off_time_ms:parseInt($('pumpSchedEndOff').value)||1000,start_trigger:parseInt($('pumpSchedStartTrig').value)||0,end_trigger:parseInt($('pumpSchedEndTrig').value)||0,start_minute:timeToMin($('pumpSchedStartTime').value),end_minute:timeToMin($('pumpSchedEndTime').value),start_offset_min:parseInt($('pumpSchedStartOffset').value)||0,end_offset_min:parseInt($('pumpSchedEndOffset').value)||0};}"
"async function savePumpSchedule(){const b=pumpScheduleBody();if(!b.target_id)return toast('Select a pump');if(/[\"\\\\<>]/.test(b.name+b.target_id))return toast('Pump schedule text cannot contain quotes, backslashes, <, or >');try{const r=await jpost('/api/pump-schedules',b);$('pumpSchedId').value=r.schedule_id||'';toast('Pump schedule saved');refreshPumpSchedules();}catch(e){toast('Pump schedule save failed: '+e.message);}}"
"async function refreshPumpSchedules(){try{const r=await jget('/api/pump-schedules');appPumpSchedules=r.schedules||[];$('pumpScheduleNext').textContent=r.next_action||'No pump action';const box=$('pumpSchedules');if(!box)return;box.innerHTML='';if(!appPumpSchedules.length){box.appendChild(el('div','empty','No pump schedules yet.'));return;}for(const s of appPumpSchedules)box.appendChild(pumpScheduleRow(s));}catch(e){toast('Pump schedules failed: '+e.message);}}"
"function pumpModeLabel(v){return({1:'Constant',2:'Lagoon',3:'Reef Crest',4:'Nutrient',5:'Tidal',6:'Short Pulse',7:'Gyre',8:'Transition',9:'Expanding Pulse',10:'Sync',12:'EcoSmart Back',13:'Feed',14:'Battery',15:'Random',16:'Pulse'})[v]||String(v);}"
"function pumpScheduleRow(s){const row=el('article','row');const left=el('div');const title=el('div','row-title');title.appendChild(el('span','name',s.name||s.schedule_id));title.appendChild(el('span','pill',s.enabled?'Enabled':'Disabled'));left.appendChild(title);addMeta(left,['Pump '+(s.target_id||''),'Start '+trigLabel(s.start_trigger)+' '+minToTime(s.start_minute||0)+' '+pumpModeLabel(s.active_mode)+' '+(s.active_speed_percent||0)+'%','End '+trigLabel(s.end_trigger)+' '+minToTime(s.end_minute||0)+' '+pumpModeLabel(s.end_mode)+' '+(s.end_speed_percent||0)+'%']);const act=el('div','actions');[['Edit',()=>loadPumpSchedule(s),'primary'],['Delete',()=>deletePumpSchedule(s.schedule_id),'danger']].forEach(a=>{const b=el('button','btn '+a[2],a[0]);b.onclick=a[1];act.appendChild(b);});row.appendChild(left);row.appendChild(act);return row;}"
"function loadPumpSchedule(s){$('pumpSchedId').value=s.schedule_id||'';$('pumpSchedEnabled').checked=!!s.enabled;$('pumpSchedName').value=s.name||'';$('pumpSchedTarget').value=s.target_id||'';$('pumpSchedActiveMode').value=s.active_mode||1;$('pumpSchedActiveSpeed').value=s.active_speed_percent||0;$('pumpSchedActiveMin').value=s.active_min_speed_percent||0;$('pumpSchedActiveVariance').value=s.active_variance_percent||0;$('pumpSchedActiveOn').value=s.active_on_time_ms||1000;$('pumpSchedActiveOff').value=s.active_off_time_ms||1000;$('pumpSchedEndMode').value=s.end_mode||1;$('pumpSchedEndSpeed').value=s.end_speed_percent||0;$('pumpSchedEndMin').value=s.end_min_speed_percent||0;$('pumpSchedEndVariance').value=s.end_variance_percent||0;$('pumpSchedEndOn').value=s.end_on_time_ms||1000;$('pumpSchedEndOff').value=s.end_off_time_ms||1000;$('pumpSchedStartTrig').value=s.start_trigger||0;$('pumpSchedEndTrig').value=s.end_trigger||0;$('pumpSchedStartTime').value=minToTime(s.start_minute||0);$('pumpSchedEndTime').value=minToTime(s.end_minute||0);$('pumpSchedStartOffset').value=s.start_offset_min||0;$('pumpSchedEndOffset').value=s.end_offset_min||0;document.querySelector('[data-tab=pumpschedules]').click();}"
"async function deletePumpSchedule(id){if(!confirm('Delete pump schedule '+id+'?'))return;try{await jdel('/api/pump-schedules/'+encodeURIComponent(id));toast('Pump schedule deleted');clearPumpScheduleForm();refreshPumpSchedules();}catch(e){toast('Delete pump schedule failed: '+e.message);}}"
"async function refreshTimeConfig(){try{const r=await jget('/api/config/time');$('timeEnabled').checked=!!r.enabled;$('timeServer').value=r.server||'time.nist.gov';$('timeTz').value=r.timezone||'UTC0';$('timeState').textContent=r.synced?'Synced':'Unsynced';$('timeNow').textContent=(r.current_local||'--')+' last sync '+(r.last_sync_local||'--');}catch(e){$('timeState').textContent='Config failed';toast('Time config failed: '+e.message);}}"
"async function saveTimeConfig(){const b={enabled:$('timeEnabled').checked,server:($('timeServer').value||'time.nist.gov').trim(),timezone:$('timeTz').value||'UTC0'};try{await jpost('/api/config/time',b);toast('Time settings saved');refreshTimeConfig();refreshStatus();}catch(e){toast('Time save failed: '+e.message);}}"
"async function refreshSunConfig(){try{const r=await jget('/api/config/sun');$('sunEnabled').checked=!!r.enabled;$('sunLabel').value=r.location_label||'My Reef';$('sunLat').value=r.latitude||0;$('sunLon').value=r.longitude||0;$('sunState').textContent=r.valid?'Ready':'Waiting';$('sunTimes').textContent='Sunrise '+(r.sunrise_local||'--:--')+' / Sunset '+(r.sunset_local||'--:--');}catch(e){$('sunState').textContent='Config failed';toast('Sun config failed: '+e.message);}}"
"async function saveSunConfig(){const b={enabled:$('sunEnabled').checked,location_label:($('sunLabel').value||'My Reef').trim(),latitude:parseFloat($('sunLat').value)||0,longitude:parseFloat($('sunLon').value)||0};try{await jpost('/api/config/sun',b);toast('Sun settings saved');refreshSunConfig();refreshSchedules();}catch(e){toast('Sun save failed: '+e.message);}}"
"function wifiModeLabel(m){return({0:'Off',1:'Station',2:'Setup AP',3:'Station + AP'})[m]||String(m);}"
"async function refreshWifiConfig(){try{const r=await jget('/api/config/wifi');$('wifiEnabled').checked=!!r.enabled;$('wifiSsid').value=r.ssid||'';$('wifiPass').value=r.password_set?'********':'';$('wifiApFallback').checked=!!r.ap_fallback_enabled;$('wifiApSsid').value=r.ap_ssid||'HydraBridge-Setup';$('wifiApPass').value=r.ap_password_set?'********':'';$('wifiState').textContent=wifiModeLabel(r.mode)+(r.connected?' Connected':'');}catch(e){$('wifiState').textContent='Config failed';toast('WiFi config failed: '+e.message);}}"
"async function saveWifiConfig(){const pass=$('wifiPass').value,apPass=$('wifiApPass').value;const b={enabled:$('wifiEnabled').checked,ssid:($('wifiSsid').value||'').trim(),ap_fallback_enabled:$('wifiApFallback').checked,ap_ssid:($('wifiApSsid').value||'HydraBridge-Setup').trim()};if(pass!=='********')b.password=pass;if(apPass!=='********')b.ap_password=apPass;try{await jpost('/api/config/wifi',b);toast('WiFi settings saved');refreshWifiConfig();refreshStatus();}catch(e){toast('WiFi save failed: '+e.message);}}"
"function modbusStatusLabel(s){return({0:'Disabled',1:'Slave Ready',2:'Master Ready',3:'Error'})[s]||String(s);}"
"async function refreshModbusConfig(){try{const r=await jget('/api/config/modbus');$('mbEnabled').checked=!!r.enabled;$('mbAddr').value=r.slave_address;$('mbBaud').value=r.baud_rate;$('mbParity').value=r.parity;$('mbUart').value=r.uart_port;$('mbTx').value=r.tx_pin;$('mbRx').value=r.rx_pin;$('mbDe').value=r.rts_de_pin;$('rs485State').textContent=modbusStatusLabel(r.status)+(r.running?'':'');}catch(e){$('rs485State').textContent='Config failed';toast('RS485 config failed: '+e.message);}}"
"async function saveModbusConfig(){const b={enabled:$('mbEnabled').checked,slave_address:parseInt($('mbAddr').value)||10,baud_rate:parseInt($('mbBaud').value)||19200,parity:parseInt($('mbParity').value)||0,uart_port:parseInt($('mbUart').value)||1,tx_pin:parseInt($('mbTx').value),rx_pin:parseInt($('mbRx').value),rts_de_pin:parseInt($('mbDe').value)};try{const r=await jpost('/api/config/modbus',b);toast(r.applied?'RS485 settings applied':'RS485 settings saved');refreshModbusConfig();refreshStatus();}catch(e){toast('RS485 save failed: '+e.message);}}"
"function mqttStatusLabel(s){return({0:'Disabled',1:'Disconnected',2:'Connected'})[s]||String(s);}"
"async function refreshMqttConfig(){try{const r=await jget('/api/config/mqtt');$('mqEnabled').checked=!!r.enabled;$('mqHost').value=r.host||'';$('mqPort').value=r.port||1883;$('mqTls').checked=!!r.use_tls;$('mqUser').value=r.username||'';$('mqPass').value=r.password_set?'********':'';$('mqClient').value=r.client_id||'';$('mqKeepalive').value=r.keepalive_sec||60;$('mqBase').value=r.base_topic||'aihydra';$('mqHa').checked=!!r.home_assistant_discovery;$('mqHaPrefix').value=r.home_assistant_prefix||'homeassistant';$('mqttState').textContent=mqttStatusLabel(r.status);}catch(e){$('mqttState').textContent='Config failed';toast('MQTT config failed: '+e.message);}}"
"async function saveMqttConfig(){const pass=$('mqPass').value;const b={enabled:$('mqEnabled').checked,host:($('mqHost').value||'').trim(),port:parseInt($('mqPort').value)||1883,use_tls:$('mqTls').checked,username:($('mqUser').value||'').trim(),client_id:($('mqClient').value||'').trim(),keepalive_sec:parseInt($('mqKeepalive').value)||60,base_topic:($('mqBase').value||'aihydra').trim(),home_assistant_discovery:$('mqHa').checked,home_assistant_prefix:($('mqHaPrefix').value||'homeassistant').trim()};if(pass!=='********')b.password=pass;try{const r=await jpost('/api/config/mqtt',b);toast(r.applied?'MQTT settings applied':'MQTT settings saved');refreshMqttConfig();refreshStatus();}catch(e){toast('MQTT save failed: '+e.message);}}"
"$('scanBtn').onclick=startScan;$('refreshLights').onclick=refreshLights;$('refreshPumps').onclick=refreshPumps;$('applyPump').onclick=()=>pumpCommand($('pumpTarget').value,pumpCommandBody());$('feedPump').onclick=()=>pumpCommand($('pumpTarget').value,{mode:13,speed_percent:10,timeout:60});$('offPump').onclick=()=>pumpCommand($('pumpTarget').value,{mode:1,speed_percent:0,timeout:60});$('saveProfile').onclick=saveProfile;$('applyCurrent').onclick=()=>applyProfile(($('profname').value||'').trim());$('intUp').onclick=()=>intensityDelta(50);$('intDown').onclick=()=>intensityDelta(-50);$('saveSchedule').onclick=saveSchedule;$('newSchedule').onclick=clearScheduleForm;$('refreshSchedules').onclick=refreshSchedules;$('savePumpSchedule').onclick=savePumpSchedule;$('newPumpSchedule').onclick=clearPumpScheduleForm;$('refreshPumpSchedules').onclick=refreshPumpSchedules;$('saveTime').onclick=saveTimeConfig;$('saveSun').onclick=saveSunConfig;$('saveWifi').onclick=saveWifiConfig;$('saveModbus').onclick=saveModbusConfig;$('saveMqtt').onclick=saveMqttConfig;"
"$('refreshGroups').onclick=refreshGroups;$('saveGroup').onclick=saveGroup;document.querySelectorAll('.tab').forEach(b=>b.onclick=()=>{document.querySelectorAll('.tab').forEach(x=>x.classList.remove('active'));document.querySelectorAll('.tabpane').forEach(x=>x.classList.remove('active'));b.classList.add('active');$('tab-'+b.dataset.tab).classList.add('active');});"
"buildChannels();clearScheduleForm();clearPumpScheduleForm();refreshStatus();refreshLights();refreshPumps();refreshGroups();refreshProfiles();refreshSchedules();refreshPumpSchedules();refreshTimeConfig();refreshSunConfig();refreshWifiConfig();refreshModbusConfig();refreshMqttConfig();setInterval(refreshStatus,3000);setInterval(refreshProfiles,10000);setInterval(refreshSchedules,10000);setInterval(refreshPumpSchedules,10000);setInterval(refreshTimeConfig,10000);setInterval(refreshSunConfig,10000);setInterval(refreshWifiConfig,10000);setInterval(refreshModbusConfig,10000);setInterval(refreshMqttConfig,10000);"
"</script></body></html>";

static esp_err_t get_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, STATUS_HTML, sizeof STATUS_HTML - 1);
}

static esp_err_t head_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t get_favicon(httpd_req_t *req)
{
    static const char FAVICON_SVG[] =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 64\">"
        "<rect width=\"64\" height=\"64\" rx=\"12\" fill=\"#172033\"/>"
        "<path d=\"M14 42c8-18 28-18 36 0\" fill=\"none\" stroke=\"#1f6feb\" stroke-width=\"7\" stroke-linecap=\"round\"/>"
        "<path d=\"M18 32c7-12 21-12 28 0\" fill=\"none\" stroke=\"#14b8a6\" stroke-width=\"6\" stroke-linecap=\"round\"/>"
        "<circle cx=\"32\" cy=\"24\" r=\"7\" fill=\"#dbeafe\"/>"
        "</svg>";
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=86400");
    return httpd_resp_send(req, FAVICON_SVG, sizeof FAVICON_SVG - 1);
}

static esp_err_t get_status(httpd_req_t *req)
{
    time_service_status_t ts;
    sun_service_status_t sun;
    schedule_engine_status_t sched;
    pump_schedule_engine_status_t pump_sched;
    time_service_get_status(&ts);
    sun_service_get_status(&sun);
    schedule_engine_get_status(&sched);
    pump_schedule_engine_get_status(&pump_sched);

    char buf[1280];
    snprintf(buf, sizeof buf,
        "{\"controller_ready\":true,"
        "\"registered_lights\":%u,"
        "\"registered_groups\":%u,"
        "\"registered_pumps\":%u,"
        "\"ble_state\":%d,"
        "\"uptime_ms\":%lu,"
        "\"time_synced\":%s,"
        "\"current_epoch\":%lld,"
        "\"current_local\":\"%s\","
        "\"last_time_sync_epoch\":%lld,"
        "\"sunrise_local\":\"%s\","
        "\"sunset_local\":\"%s\","
        "\"next_schedule_action\":\"%s\","
        "\"next_pump_schedule_action\":\"%s\"}",
        (unsigned)light_registry_count(),
        (unsigned)group_registry_count(),
        (unsigned)pump_registry_count(),
        (int)ble_light_client_current_state(),
        (unsigned long)esp_log_timestamp(),
        ts.synced ? "true" : "false",
        (long long)ts.current_epoch,
        ts.current_local,
        (long long)ts.last_sync_epoch,
        sun.sunrise_local,
        sun.sunset_local,
        sched.next_action,
        pump_sched.next_action);
    return send_json(req, buf);
}

/* ===== scan ===== */

static bool build_hydra_light_from_result(const ble_scan_result_t *r, registered_light_t *out);
static bool looks_like_hydra(const ble_scan_result_t *r);

static bool looks_like_orbit_pump(const ble_scan_result_t *r)
{
    if (!r) return false;
    if (r->manuf.model == 259 || r->manuf.model == 260) return true;
    if (strstr(r->name, "Orbit")) return true;
    return false;
}

static bool has_known_non_hydra_model(const ble_scan_result_t *r)
{
    if (!r || !r->has_manuf_data || r->manuf.model == 0) return false;
    return !ble_scanner_is_ai_model(r->manuf.model) &&
           r->manuf.model != 259 &&
           r->manuf.model != 260;
}

static bool looks_like_supported_scan_result(const ble_scan_result_t *r)
{
    return looks_like_orbit_pump(r) || looks_like_hydra(r);
}

static void scan_cb(const ble_scan_result_t *result, void *user_ctx)
{
    (void)user_ctx;
    if (!result) return;

    /* Claim hydra lights *immediately* on sighting during the user scan, but ONLY
     * when the report actually carried the manufacturer data (has_manuf=1, v1/v2).
     * Observed pairing/manualDiscovery identification is from the 0xFF manuf
     * payload (v2 02 + flags bit 3 for manualDiscovery), not just the service UUID.
     * We stop the scan and connect immediately with the fresh addr from *that* report
     * to capture the current (possibly RPA) addr before it rotates or the pairing
     * adv window closes. This is why phone finds it fast (gets full scanRecord with
     * both adv + rsp in one go) but delayed connect after full scan fails.
     * Only claims if new (add succeeds); dups just continue (or update buf). */
    registered_light_t l;
    if (result->has_manuf_data && build_hydra_light_from_result(result, &l)) {
        int rc = light_registry_add(&l);
        if (rc == 0) {
            light_registry_save();
            ble_scanner_stop();
            try_connect_to(&l, false);  /* hot connect: skip extra prime, use the sighting itself as the recent SCAN_REQ */
            /* fall through so this sighting is still recorded in the scan results */
        } else {
            refresh_registered_peer(&l, result->rssi);
        }
    }

    if (!looks_like_supported_scan_result(result)) return;

    for (size_t i = 0; i < s_scan_count; ++i) {
        if (memcmp(s_scan_buf[i].ble_addr, result->ble_addr, BLE_ADDR_BYTES) == 0) {
            merge_scan_result(&s_scan_buf[i], result);
            return;
        }
    }
    if (s_scan_count >= SCAN_BUF_MAX) return;
    s_scan_buf[s_scan_count++] = *result;
}

static esp_err_t post_scan(httpd_req_t *req)
{
    int st = ble_light_client_current_state();
    /* Allow user scans for *new* pairing lights even if a previously-registered
     * light is in BACKOFF/ERROR (dead light, pairing mode, etc.). Only block
     * during active link use. The long backoff in light_client prevents spam. */
    if (st == BLC_STATE_CONNECTING || st == BLC_STATE_DISCOVERING ||
        st == BLC_STATE_WRITING || st == BLC_STATE_WAITING_CONFIRM) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ble busy; try again in a few seconds");
        return ESP_FAIL;
    }
    s_scan_count = 0;
    memset(s_scan_buf, 0, sizeof s_scan_buf);
    ble_scanner_init();
    ble_scanner_stop();  /* ensure any prior scan flag is cleared before starting fresh user scan */
    esp_err_t err = ble_scanner_start(scan_cb, NULL, SCAN_DURATION_MS);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan start failed");
        return ESP_FAIL;
    }
    return send_json(req, "{\"started\":true,\"duration_ms\":30000}");
}

static bool looks_like_hydra(const ble_scan_result_t *r)
{
    if (!r) return false;
    if (looks_like_orbit_pump(r)) return false;
    if (has_known_non_hydra_model(r)) return false;
    if (r->has_manuf_data) {
        if (ble_scanner_is_ai_model(r->manuf.model)) return true;
    }
    if (strstr(r->name, "Hydra")) return true;
    if (r->has_hydra_service && (!r->has_manuf_data || strstr(r->name, "MOBIUS"))) return true;
    return false;
}

static bool build_hydra_light_from_result(const ble_scan_result_t *r, registered_light_t *out)
{
    if (!r || !out || !looks_like_hydra(r)) return false;
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    out->ble_addr_type = r->ble_addr_type;
    memcpy(out->ble_addr, r->ble_addr, sizeof out->ble_addr);

    uint16_t m = r->manuf.model ? r->manuf.model : 335;
    out->model = m;

    char base[32] = {0};
    if (r->manuf.serial[0] && m != 0) {
        strncpy(base, r->manuf.serial, sizeof(base)-1);
    } else {
        for (int j = 0; j < 6; j++) {
            char tmp[3] = {0};
            snprintf(tmp, 3, "%02X", r->ble_addr[j]);
            strncat(base, tmp, sizeof(base) - strlen(base) - 1);
        }
        if (strlen(base) > 10) base[10] = 0;
    }

    // clean to alnum only (match JS derivation for lid)
    char cleaned[32] = {0};
    size_t ci = 0;
    for (char *p = base; *p && ci < sizeof(cleaned)-1; p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            cleaned[ci++] = *p;
        }
    }

    snprintf(out->light_id, sizeof out->light_id, "hydra64-%.20s", cleaned);

    if (r->name[0]) {
        strncpy(out->display_name, r->name, sizeof out->display_name - 1);
    } else {
        snprintf(out->display_name, sizeof out->display_name,
                 (m == 0 ? "Unknown (pairing)" : "Hydra %u"), (unsigned)m);
    }

    if (r->manuf.serial[0] && m != 0) {
        strncpy(out->serial, r->manuf.serial, sizeof out->serial - 1);
    } else {
        hydra_ble_addr_format(r->ble_addr, out->serial, sizeof out->serial);
    }
    return true;
}

static void auto_add_hydra_lights(void)
{
    for (size_t i = 0; i < s_scan_count; ++i) {
        const ble_scan_result_t *r = &s_scan_buf[i];
        registered_light_t l;
        if (build_hydra_light_from_result(r, &l)) {
            int rc = light_registry_add(&l);
            if (rc == 0) {
                light_registry_save();
                try_connect_to(&l, true);
            } else {
                refresh_registered_peer(&l, r->rssi);
            }
        }
    }
}

static esp_err_t get_scan_results(httpd_req_t *req)
{
    auto_add_hydra_lights();  /* automatically register & connect any hydra lights found in this scan (pairing or not) */

    char buf[4096];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"results\":[");
    for (size_t i = 0; i < s_scan_count && off < sizeof buf - 200; ++i) {
        const ble_scan_result_t *r = &s_scan_buf[i];
        char addr[24];
        hydra_ble_addr_format(r->ble_addr, addr, sizeof addr);
        const char *device_type = looks_like_orbit_pump(r) ? "pump" : (looks_like_hydra(r) ? "light" : "unknown");
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"name\":\"%s\",\"ble_addr\":\"%s\",\"ble_addr_type\":%d,\"rssi\":%d,"
            "\"model\":%u,\"serial\":\"%s\",\"device_type\":\"%s\",\"supported\":%s}",
            i == 0 ? "" : ",",
            r->name, addr, (int)r->ble_addr_type, r->rssi,
            (unsigned)r->manuf.model, r->manuf.serial, device_type,
            strcmp(device_type, "unknown") == 0 ? "false" : "true");
    }
    snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

/* ===== lights ===== */

static esp_err_t get_lights(httpd_req_t *req)
{
    char buf[1024];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"lights\":[");
    size_t n = light_registry_count();
    for (size_t i = 0; i < n && off < sizeof buf - 200; ++i) {
        const registered_light_t *l = light_registry_at(i);
        if (!l) continue;
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"light_id\":\"%s\",\"display_name\":\"%s\","
            "\"serial\":\"%s\",\"model\":%u,\"enabled\":%s,"
            "\"last_seen_rssi\":%d}",
            i == 0 ? "" : ",",
            l->light_id, l->display_name, l->serial,
            (unsigned)l->model,
            l->enabled ? "true" : "false",
            l->last_seen_rssi);
    }
    snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

static int json_get_str(const char *json, const char *key, char *out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '"') return -1;
    ++p;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) { out[i++] = *p++; }
    out[i] = '\0';
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (!((*p >= '0' && *p <= '9') || *p == '-')) return -1;
    *out = (int)strtol(p, NULL, 10);
    return 0;
}

static int json_get_bool(const char *json, const char *key, bool *out)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return 0;
    }
    int v = 0;
    if (json_get_int(json, key, &v) == 0) {
        *out = (v != 0);
        return 0;
    }
    return -1;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex8(const char *text, uint8_t out[8])
{
    if (!text || !out) return -1;
    size_t n = strlen(text);
    if (n != 16) return -1;
    for (size_t i = 0; i < 8; ++i) {
        int hi = hex_nibble(text[i * 2]);
        int lo = hex_nibble(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static bool pump_mode_ui_supported(uint8_t mode)
{
    return mode == CONFIG_PUMP_MODE_CONSTANT ||
           mode == CONFIG_PUMP_MODE_RANDOM ||
           mode == CONFIG_PUMP_MODE_PULSE ||
           mode == CONFIG_PUMP_MODE_FEED;
}

static bool pump_command_values_ok(const pending_command_t *cmd)
{
    if (!cmd) return false;
    if (cmd->pump_speed_percent > 100 ||
        cmd->pump_min_speed_percent > 100 ||
        cmd->pump_variance_percent > 100) return false;
    if (!ai_pump_mode_is_orbit_supported(cmd->pump_mode)) return false;
    if (cmd->pump_mode == CONFIG_PUMP_MODE_SYNC && !cmd->pump_has_master) return false;
    if (cmd->pump_mode == CONFIG_PUMP_MODE_PULSE &&
        (cmd->pump_on_time_ms == 0 || cmd->pump_off_time_ms == 0)) return false;
    return true;
}

static int json_get_double_scaled_e7(const char *json, const char *key, int32_t *out)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return -1;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    char *end = NULL;
    double v = strtod(p, &end);
    if (end == p) return -1;
    if (v < -180.0 || v > 180.0) return -1;
    *out = (int32_t)(v * 10000000.0 + (v >= 0 ? 0.5 : -0.5));
    return 0;
}

static void format_e7(int32_t v, char out[24])
{
    if (!out) return;
    int64_t av = v < 0 ? -(int64_t)v : (int64_t)v;
    snprintf(out, 24, "%s%lld.%07lld",
             v < 0 ? "-" : "",
             (long long)(av / 10000000LL),
             (long long)(av % 10000000LL));
}

static bool safe_display_name(const char *s)
{
    if (!s || !s[0]) return false;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || *p == '"' || *p == '\\' || *p == '<' || *p == '>') return false;
    }
    return true;
}

static bool safe_profile_text(const char *s, bool allow_empty)
{
    if (!s) return false;
    if (!allow_empty && !s[0]) return false;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || *p == '"' || *p == '\\' || *p == '<' || *p == '>') {
            return false;
        }
    }
    return true;
}

static bool safe_mqtt_text(const char *s, bool allow_empty, bool allow_slash)
{
    if (!s) return false;
    if (!allow_empty && !s[0]) return false;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c > 0x7e) return false;
        if (*p == '"' || *p == '\\' || *p == '<' || *p == '>') return false;
        if (*p == '#' || *p == '+') return false;
        if (!allow_slash && *p == '/') return false;
    }
    return true;
}

static esp_err_t post_lights(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    registered_light_t l;
    memset(&l, 0, sizeof l);
    char addr_str[24] = {0};
    int model = 335;

    if (json_get_str(body, "light_id",     l.light_id,     sizeof l.light_id) != 0 ||
        json_get_str(body, "display_name", l.display_name, sizeof l.display_name) != 0 ||
        json_get_str(body, "ble_addr",     addr_str,       sizeof addr_str) != 0 ||
        json_get_str(body, "serial",       l.serial,       sizeof l.serial) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        return ESP_FAIL;
    }
    json_get_int(body, "model", &model);
    l.model = (uint16_t)model;
    l.enabled = true;
    int addr_type = BLE_ADDR_PUBLIC;
    json_get_int(body, "ble_addr_type", &addr_type);
    if (addr_type == BLE_ADDR_RANDOM) {
        l.ble_addr_type = BLE_ADDR_RANDOM;
    } else if (addr_type == BLE_ADDR_PUBLIC_ID) {
        l.ble_addr_type = BLE_ADDR_PUBLIC_ID;
    } else if (addr_type == BLE_ADDR_RANDOM_ID) {
        l.ble_addr_type = BLE_ADDR_RANDOM_ID;
    } else {
        l.ble_addr_type = BLE_ADDR_PUBLIC;
    }
    if (hydra_ble_addr_parse(addr_str, l.ble_addr) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ble_addr");
        return ESP_FAIL;
    }

    int rc = light_registry_add(&l);
    if (rc != 0) {
        const registered_light_t *existing = light_registry_get(l.light_id);
        if (!existing) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "duplicate or full");
            return ESP_FAIL;
        }
        if (light_registry_update_discovery(existing->light_id, l.ble_addr,
                                            l.ble_addr_type, l.last_seen_rssi) != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update failed");
            return ESP_FAIL;
        }
    }
    light_registry_save();

    /* For lights added from a fresh scan result (especially pairing mode with
     * the v2 02 0a manualDiscovery manuf), immediately prime + connect using
     * the *exact* addr we just received. This keeps the attempt as close as
     * possible to the scan time (when the light was advertising the pairing
     * data and accepting connects). The normal worker will take over once
     * connected. This is why "hard-coded" worked but delayed worker-driven
     * connect after UI add sometimes didn't (addr rotation / narrow window). */
    try_connect_to(&l, true);

    char out[160];
    snprintf(out, sizeof out, "{\"added\":%s,\"light_id\":\"%s\"}",
             rc == 0 ? "true" : "false", l.light_id);
    return send_json(req, out);
}

static esp_err_t delete_light(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/lights/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/lights/");
    char light_id[LIGHT_ID_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < LIGHT_ID_LEN) { light_id[i] = p[i]; ++i; }
    light_id[i] = '\0';
    if (i == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing light_id"); return ESP_FAIL; }

    int rc = light_registry_remove(light_id);
    if (rc != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    light_registry_save();
    char out[96];
    snprintf(out, sizeof out, "{\"removed\":true,\"light_id\":\"%s\"}", light_id);
    return send_json(req, out);
}

/* ===== pumps ===== */

static esp_err_t get_pumps(httpd_req_t *req)
{
    char buf[1536];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"pumps\":[");
    size_t n = pump_registry_count();
    for (size_t i = 0; i < n && off < sizeof buf - 240; ++i) {
        const registered_pump_t *p = pump_registry_at(i);
        if (!p) continue;
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"pump_id\":\"%s\",\"display_name\":\"%s\","
            "\"serial\":\"%s\",\"model\":%u,\"enabled\":%s,"
            "\"last_seen_rssi\":%d,\"last_mode\":%u,"
            "\"last_speed_percent\":%u,\"protocol_status\":%u}",
            i == 0 ? "" : ",",
            p->pump_id, p->display_name, p->serial,
            (unsigned)p->model,
            p->enabled ? "true" : "false",
            p->last_seen_rssi,
            (unsigned)p->last_mode,
            (unsigned)p->last_speed_percent,
            (unsigned)p->protocol_status);
    }
    snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

static esp_err_t post_pumps(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    registered_pump_t p;
    memset(&p, 0, sizeof p);
    char addr_str[24] = {0};
    int model = 0;

    if (json_get_str(body, "pump_id", p.pump_id, sizeof p.pump_id) != 0 ||
        json_get_str(body, "display_name", p.display_name, sizeof p.display_name) != 0 ||
        json_get_str(body, "ble_addr", addr_str, sizeof addr_str) != 0 ||
        json_get_str(body, "serial", p.serial, sizeof p.serial) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing fields");
        return ESP_FAIL;
    }
    json_get_int(body, "model", &model);
    p.model = (uint16_t)model;
    p.enabled = true;
    p.last_mode = PUMP_MODE_CONSTANT;
    p.last_speed_percent = 0;
    p.protocol_status = PUMP_PROTOCOL_EXPERIMENTAL_MOBIUS;
    int addr_type = BLE_ADDR_PUBLIC;
    json_get_int(body, "ble_addr_type", &addr_type);
    if (addr_type == BLE_ADDR_RANDOM) {
        p.ble_addr_type = BLE_ADDR_RANDOM;
    } else if (addr_type == BLE_ADDR_PUBLIC_ID) {
        p.ble_addr_type = BLE_ADDR_PUBLIC_ID;
    } else if (addr_type == BLE_ADDR_RANDOM_ID) {
        p.ble_addr_type = BLE_ADDR_RANDOM_ID;
    } else {
        p.ble_addr_type = BLE_ADDR_PUBLIC;
    }
    if (hydra_ble_addr_parse(addr_str, p.ble_addr) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad ble_addr");
        return ESP_FAIL;
    }
    if (!safe_profile_text(p.pump_id, false) ||
        !safe_display_name(p.display_name) ||
        !safe_profile_text(p.serial, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pump fields");
        return ESP_FAIL;
    }
    if (light_registry_get(p.pump_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pump_id collides with light_id");
        return ESP_FAIL;
    }

    int rc = pump_registry_add(&p);
    if (rc != 0) {
        const registered_pump_t *existing = pump_registry_get(p.pump_id);
        if (!existing) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "duplicate or full");
            return ESP_FAIL;
        }
        if (pump_registry_update_discovery(existing->pump_id, p.ble_addr,
                                           p.ble_addr_type, p.last_seen_rssi) != 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "update failed");
            return ESP_FAIL;
        }
    }
    pump_registry_save();

    char out[160];
    snprintf(out, sizeof out, "{\"added\":%s,\"pump_id\":\"%s\",\"experimental\":true}",
             rc == 0 ? "true" : "false", p.pump_id);
    return send_json(req, out);
}

static esp_err_t delete_pump(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/pumps/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/pumps/");
    char pump_id[PUMP_ID_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < PUMP_ID_LEN) { pump_id[i] = p[i]; ++i; }
    pump_id[i] = '\0';
    if (i == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing pump_id"); return ESP_FAIL; }

    if (pump_registry_remove(pump_id) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    pump_registry_save();
    char out[96];
    snprintf(out, sizeof out, "{\"removed\":true,\"pump_id\":\"%s\"}", pump_id);
    return send_json(req, out);
}

static esp_err_t post_pump_command(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/pumps/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/pumps/");
    const char *slash = strchr(p, '/');
    if (!slash) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing pump_id"); return ESP_FAIL; }
    char pump_id[PUMP_ID_LEN];
    size_t id_len = slash - p;
    if (id_len >= PUMP_ID_LEN) id_len = PUMP_ID_LEN - 1;
    memcpy(pump_id, p, id_len);
    pump_id[id_len] = '\0';

    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    if (strcmp(slash, "/rename") == 0) {
        char display_name[PUMP_NAME_LEN];
        if (json_get_str(body, "display_name", display_name, sizeof display_name) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing display_name");
            return ESP_FAIL;
        }
        if (!safe_display_name(display_name)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad display_name");
            return ESP_FAIL;
        }
        if (pump_registry_rename(pump_id, display_name) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rename failed");
            return ESP_FAIL;
        }
        pump_registry_save();
        const registered_pump_t *pump = pump_registry_get(pump_id);
        char out[128];
        snprintf(out, sizeof out,
                 "{\"renamed\":true,\"pump_id\":\"%s\",\"display_name\":\"%s\"}",
                 pump_id, pump ? pump->display_name : display_name);
        return send_json(req, out);
    }

    if (strcmp(slash, "/command") != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown pump subpath");
        return ESP_FAIL;
    }

    const registered_pump_t *pump = pump_registry_get(pump_id);
    if (!pump || !pump->enabled) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "pump not found");
        return ESP_FAIL;
    }
    int mode = CONFIG_PUMP_MODE_CONSTANT;
    int speed = 0;
    int min_speed = 0;
    int variance = 0;
    int on_ms = 1000;
    int off_ms = 1000;
    int pulse_ms = 1000;
    int start_ms = 1000;
    int end_ms = 1000;
    int phase = 0;
    int timeout = 60;
    json_get_int(body, "mode", &mode);
    json_get_int(body, "speed_percent", &speed);
    json_get_int(body, "min_speed_percent", &min_speed);
    json_get_int(body, "variance_percent", &variance);
    json_get_int(body, "on_time_ms", &on_ms);
    json_get_int(body, "off_time_ms", &off_ms);
    json_get_int(body, "pulse_time_ms", &pulse_ms);
    json_get_int(body, "start_time_ms", &start_ms);
    json_get_int(body, "end_time_ms", &end_ms);
    json_get_int(body, "phase_shift_deg", &phase);
    json_get_int(body, "timeout", &timeout);
    if (speed < 0 || speed > 100 ||
        min_speed < 0 || min_speed > 100 ||
        variance < 0 || variance > 100 ||
        on_ms < 0 || off_ms < 0 || pulse_ms < 0 ||
        start_ms < 0 || start_ms > 65535 ||
        end_ms < 0 || end_ms > 65535 ||
        phase < 0 || phase > 65535 ||
        timeout < 0 || timeout > 3600) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pump command");
        return ESP_FAIL;
    }

    pending_command_t cmd;
    memset(&cmd, 0, sizeof cmd);
    snprintf(cmd.command_id, sizeof cmd.command_id, "web-pump-%lu", (unsigned long)esp_log_timestamp());
    cmd.source = CMD_SOURCE_WEB;
    cmd.type = CMD_TYPE_PUMP_SET;
    cmd.device_type = CMD_DEVICE_PUMP;
    strncpy(cmd.light_id, pump_id, sizeof cmd.light_id - 1);
    cmd.timeout_ms = 30000;
    cmd.scene_timeout_sec = (uint16_t)timeout;
    cmd.pump_mode = (uint8_t)mode;
    cmd.pump_speed_percent = (uint8_t)speed;
    cmd.pump_min_speed_percent = (uint8_t)min_speed;
    cmd.pump_variance_percent = (uint8_t)variance;
    cmd.pump_on_time_ms = (uint32_t)on_ms;
    cmd.pump_off_time_ms = (uint32_t)off_ms;
    cmd.pump_pulse_time_ms = (uint32_t)pulse_ms;
    cmd.pump_start_time_ms = (uint16_t)start_ms;
    cmd.pump_end_time_ms = (uint16_t)end_ms;
    cmd.pump_phase_shift_deg = (uint16_t)phase;

    char master_hex[24] = {0};
    if (json_get_str(body, "master_hex", master_hex, sizeof master_hex) == 0) {
        if (parse_hex8(master_hex, cmd.pump_master) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad master_hex");
            return ESP_FAIL;
        }
        cmd.pump_has_master = true;
    }

    if (!pump_command_values_ok(&cmd)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pump command");
        return ESP_FAIL;
    }
    if (cmd_queue_push(&cmd) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "pump queue full");
        return ESP_FAIL;
    }

    char out[160];
    snprintf(out, sizeof out,
             "{\"command_id\":\"%s\",\"queued\":true,\"experimental\":true}",
             cmd.command_id);
    return send_json(req, out);
}

/* ===== groups ===== */

static esp_err_t get_groups(httpd_req_t *req)
{
    char buf[2048];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"groups\":[");
    size_t n = group_registry_count();
    for (size_t i = 0; i < n && off < sizeof buf - 300; ++i) {
        const light_group_t *g = group_registry_at(i);
        if (!g) continue;
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"group_id\":\"%s\",\"display_name\":\"%s\",\"enabled\":%s,"
            "\"member_count\":%u,\"members\":[",
            i == 0 ? "" : ",", g->group_id, g->display_name,
            g->enabled ? "true" : "false", (unsigned)g->member_count);
        for (uint8_t m = 0; m < g->member_count && off < sizeof buf - 80; ++m) {
            off += snprintf(buf + off, sizeof buf - off,
                            "%s\"%s\"", m == 0 ? "" : ",", g->light_ids[m]);
        }
        off += snprintf(buf + off, sizeof buf - off, "]}");
    }
    snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

static void make_group_id_from_name(const char *name, char out[GROUP_ID_LEN])
{
    strncpy(out, "grp-", GROUP_ID_LEN - 1);
    size_t off = 4;
    for (const char *p = name; p && *p && off + 1 < GROUP_ID_LEN; ++p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
            out[off++] = (char)c;
        }
    }
    out[off] = '\0';
}

static esp_err_t post_groups(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    light_group_t g;
    memset(&g, 0, sizeof g);
    g.enabled = true;
    g.fanout_mode = LIGHT_FANOUT_SEQUENTIAL;

    if (json_get_str(body, "display_name", g.display_name, sizeof g.display_name) != 0 ||
        !safe_display_name(g.display_name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad display_name");
        return ESP_FAIL;
    }
    if (json_get_str(body, "group_id", g.group_id, sizeof g.group_id) != 0 || g.group_id[0] == '\0') {
        make_group_id_from_name(g.display_name, g.group_id);
    }
    if (!safe_profile_text(g.group_id, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad group_id");
        return ESP_FAIL;
    }

    char members[256] = {0};
    if (json_get_str(body, "members", members, sizeof members) != 0 || members[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing members");
        return ESP_FAIL;
    }
    char *save = NULL;
    for (char *tok = strtok_r(members, ",", &save);
         tok && g.member_count < LIGHT_GROUP_MEMBERS_MAX;
         tok = strtok_r(NULL, ",", &save)) {
        while (*tok == ' ' || *tok == '\t') ++tok;
        if (!light_registry_get(tok)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "member light not found");
            return ESP_FAIL;
        }
        strncpy(g.light_ids[g.member_count], tok, LIGHT_ID_LEN - 1);
        g.light_ids[g.member_count][LIGHT_ID_LEN - 1] = '\0';
        g.member_count++;
    }
    if (g.member_count == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty group");
        return ESP_FAIL;
    }

    if (group_registry_get(g.group_id)) {
        group_registry_remove(g.group_id);
    }
    if (group_registry_add(&g) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "group add failed");
        return ESP_FAIL;
    }
    light_registry_save();
    char out[128];
    snprintf(out, sizeof out, "{\"saved\":true,\"group_id\":\"%s\"}", g.group_id);
    return send_json(req, out);
}

static esp_err_t delete_group(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/groups/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/groups/");
    char group_id[GROUP_ID_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < GROUP_ID_LEN) { group_id[i] = p[i]; ++i; }
    group_id[i] = '\0';
    if (i == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing group_id"); return ESP_FAIL; }
    if (group_registry_remove(group_id) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    light_registry_save();
    char out[96];
    snprintf(out, sizeof out, "{\"deleted\":true,\"group_id\":\"%s\"}", group_id);
    return send_json(req, out);
}

/* ===== RS485 / Modbus config ===== */

static const char *parity_name(modbus_parity_t p)
{
    switch (p) {
        case MODBUS_PARITY_EVEN: return "even";
        case MODBUS_PARITY_ODD:  return "odd";
        case MODBUS_PARITY_NONE:
        default:                 return "none";
    }
}

static esp_err_t get_modbus_config(httpd_req_t *req)
{
    config_modbus_t mb;
    config_store_load_modbus(&mb);
    char buf[384];
    snprintf(buf, sizeof buf,
        "{\"enabled\":%s,"
        "\"running\":%s,"
        "\"status\":%u,"
        "\"slave_address\":%u,"
        "\"baud_rate\":%lu,"
        "\"data_bits\":%u,"
        "\"parity\":%d,"
        "\"parity_name\":\"%s\","
        "\"stop_bits\":%u,"
        "\"uart_port\":%u,"
        "\"tx_pin\":%d,"
        "\"rx_pin\":%d,"
        "\"rts_de_pin\":%d,"
        "\"response_timeout_ms\":%lu,"
        "\"command_watchdog_ms\":%lu}",
        mb.enabled ? "true" : "false",
        modbus_slave_driver_is_running() ? "true" : "false",
        (unsigned)modbus_store_get(HYDRA_MODBUS_REG_MODBUS_STATUS),
        (unsigned)mb.slave_address,
        (unsigned long)mb.baud_rate,
        (unsigned)mb.data_bits,
        (int)mb.parity,
        parity_name(mb.parity),
        (unsigned)mb.stop_bits,
        (unsigned)mb.uart_port,
        (int)mb.tx_pin,
        (int)mb.rx_pin,
        (int)mb.rts_de_pin,
        (unsigned long)mb.response_timeout_ms,
        (unsigned long)mb.command_watchdog_ms);
    return send_json(req, buf);
}

static bool valid_gpio_or_disabled(int pin)
{
    return pin == -1 || (pin >= 0 && pin <= 48);
}

static esp_err_t post_modbus_config(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_modbus_t mb;
    config_store_load_modbus(&mb);

    bool enabled = false;
    if (json_get_bool(body, "enabled", &enabled) == 0) mb.enabled = enabled;

    int v = 0;
    if (json_get_int(body, "slave_address", &v) == 0) mb.slave_address = (uint8_t)v;
    if (json_get_int(body, "baud_rate", &v) == 0) mb.baud_rate = (uint32_t)v;
    if (json_get_int(body, "parity", &v) == 0) mb.parity = (modbus_parity_t)v;
    if (json_get_int(body, "uart_port", &v) == 0) mb.uart_port = (uint8_t)v;
    if (json_get_int(body, "tx_pin", &v) == 0) mb.tx_pin = (int8_t)v;
    if (json_get_int(body, "rx_pin", &v) == 0) mb.rx_pin = (int8_t)v;
    if (json_get_int(body, "rts_de_pin", &v) == 0) mb.rts_de_pin = (int8_t)v;
    mb.master_mode_enabled = false;
    mb.data_bits = 8;
    mb.stop_bits = 1;

    if (mb.slave_address < 1 || mb.slave_address > 247) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "slave_address must be 1..247");
        return ESP_FAIL;
    }
    if (mb.baud_rate < 1200 || mb.baud_rate > 921600) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "baud_rate out of range");
        return ESP_FAIL;
    }
    if (!(mb.parity == MODBUS_PARITY_NONE || mb.parity == MODBUS_PARITY_EVEN ||
          mb.parity == MODBUS_PARITY_ODD)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad parity");
        return ESP_FAIL;
    }
    if (mb.uart_port > 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "uart_port must be 0..2");
        return ESP_FAIL;
    }
    if (!valid_gpio_or_disabled(mb.tx_pin) || !valid_gpio_or_disabled(mb.rx_pin) ||
        !valid_gpio_or_disabled(mb.rts_de_pin)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pin must be -1 or 0..48");
        return ESP_FAIL;
    }
    if (mb.enabled && (mb.tx_pin < 0 || mb.rx_pin < 0 || mb.rts_de_pin < 0)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled RS485 requires tx_pin, rx_pin, and rts_de_pin");
        return ESP_FAIL;
    }

    esp_err_t err = config_store_save_modbus(&mb);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }

    err = modbus_interface_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }

    char out[128];
    snprintf(out, sizeof out,
             "{\"saved\":true,\"applied\":true,\"enabled\":%s,\"running\":%s,\"status\":%u}",
             mb.enabled ? "true" : "false",
             modbus_slave_driver_is_running() ? "true" : "false",
             (unsigned)modbus_store_get(HYDRA_MODBUS_REG_MODBUS_STATUS));
    return send_json(req, out);
}

/* ===== WiFi config ===== */

static esp_err_t get_wifi_config(httpd_req_t *req)
{
    config_wifi_t w;
    config_store_load_wifi(&w);

    char buf[512];
    snprintf(buf, sizeof buf,
        "{\"enabled\":%s,"
        "\"connected\":%s,"
        "\"ap_active\":%s,"
        "\"mode\":%d,"
        "\"ssid\":\"%s\","
        "\"password_set\":%s,"
        "\"ap_fallback_enabled\":%s,"
        "\"ap_ssid\":\"%s\","
        "\"ap_password_set\":%s,"
        "\"setup_url\":\"http://192.168.1.10/\"}",
        w.enabled ? "true" : "false",
        hydra_wifi_is_connected() ? "true" : "false",
        hydra_wifi_ap_is_active() ? "true" : "false",
        (int)hydra_wifi_mode(),
        w.ssid,
        w.password[0] ? "true" : "false",
        w.ap_fallback_enabled ? "true" : "false",
        w.ap_ssid,
        w.ap_password[0] ? "true" : "false");
    return send_json(req, buf);
}

static esp_err_t post_wifi_config(httpd_req_t *req)
{
    char body[768];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_wifi_t w;
    config_store_load_wifi(&w);

    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) w.enabled = b;
    if (json_get_bool(body, "ap_fallback_enabled", &b) == 0) w.ap_fallback_enabled = b;

    char s[CONFIG_WIFI_PSK_LEN];
    if (json_get_str(body, "ssid", s, sizeof s) == 0) {
        strncpy(w.ssid, s, sizeof w.ssid - 1);
        w.ssid[sizeof w.ssid - 1] = '\0';
    }
    if (json_get_str(body, "password", s, sizeof s) == 0 && strcmp(s, "********") != 0) {
        strncpy(w.password, s, sizeof w.password - 1);
        w.password[sizeof w.password - 1] = '\0';
    }
    if (json_get_str(body, "ap_ssid", s, sizeof s) == 0) {
        strncpy(w.ap_ssid, s, sizeof w.ap_ssid - 1);
        w.ap_ssid[sizeof w.ap_ssid - 1] = '\0';
    }
    if (json_get_str(body, "ap_password", s, sizeof s) == 0 && strcmp(s, "********") != 0) {
        strncpy(w.ap_password, s, sizeof w.ap_password - 1);
        w.ap_password[sizeof w.ap_password - 1] = '\0';
    }

    if (w.ap_fallback_enabled && w.ap_ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP fallback requires ap_ssid");
        return ESP_FAIL;
    }
    if (!safe_profile_text(w.ssid, true) ||
        !safe_profile_text(w.password, true) ||
        !safe_profile_text(w.ap_ssid, !w.ap_fallback_enabled) ||
        !safe_profile_text(w.ap_password, true)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad WiFi text");
        return ESP_FAIL;
    }
    if (w.ap_password[0] && strlen(w.ap_password) < 8) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "AP password must be blank or at least 8 chars");
        return ESP_FAIL;
    }

    esp_err_t err = config_store_save_wifi(&w);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    err = hydra_wifi_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }

    char out[160];
    snprintf(out, sizeof out,
             "{\"saved\":true,\"applied\":true,\"connected\":%s,\"ap_active\":%s,\"mode\":%d}",
             hydra_wifi_is_connected() ? "true" : "false",
             hydra_wifi_ap_is_active() ? "true" : "false",
             (int)hydra_wifi_mode());
    return send_json(req, out);
}

/* ===== MQTT config ===== */

static esp_err_t get_mqtt_config(httpd_req_t *req)
{
    config_mqtt_t mq;
    config_store_load_mqtt(&mq);

    uint16_t status = modbus_store_get(HYDRA_MODBUS_REG_MQTT_STATUS);
    char buf[640];
    snprintf(buf, sizeof buf,
        "{\"enabled\":%s,"
        "\"connected\":%s,"
        "\"status\":%u,"
        "\"host\":\"%s\","
        "\"port\":%u,"
        "\"use_tls\":%s,"
        "\"username\":\"%s\","
        "\"password_set\":%s,"
        "\"client_id\":\"%s\","
        "\"keepalive_sec\":%u,"
        "\"base_topic\":\"%s\","
        "\"home_assistant_discovery\":%s,"
        "\"home_assistant_prefix\":\"%s\"}",
        mq.enabled ? "true" : "false",
        mqtt_bridge_is_connected() ? "true" : "false",
        (unsigned)status,
        mq.host,
        (unsigned)mq.port,
        mq.use_tls ? "true" : "false",
        mq.username,
        mq.password[0] ? "true" : "false",
        mq.client_id,
        (unsigned)mq.keepalive_sec,
        mq.base_topic,
        mq.home_assistant_discovery ? "true" : "false",
        mq.home_assistant_prefix);
    return send_json(req, buf);
}

static esp_err_t post_mqtt_config(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_mqtt_t mq;
    config_store_load_mqtt(&mq);

    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) mq.enabled = b;
    if (json_get_bool(body, "use_tls", &b) == 0) mq.use_tls = b;
    if (json_get_bool(body, "home_assistant_discovery", &b) == 0) {
        mq.home_assistant_discovery = b;
    }

    char s[CONFIG_MQTT_PSK_LEN];
    if (json_get_str(body, "host", s, sizeof s) == 0) {
        strncpy(mq.host, s, sizeof mq.host - 1);
        mq.host[sizeof mq.host - 1] = '\0';
    }
    if (json_get_str(body, "username", s, sizeof s) == 0) {
        strncpy(mq.username, s, sizeof mq.username - 1);
        mq.username[sizeof mq.username - 1] = '\0';
    }
    if (json_get_str(body, "password", s, sizeof s) == 0 && strcmp(s, "********") != 0) {
        strncpy(mq.password, s, sizeof mq.password - 1);
        mq.password[sizeof mq.password - 1] = '\0';
    }
    if (json_get_str(body, "client_id", s, sizeof s) == 0) {
        strncpy(mq.client_id, s, sizeof mq.client_id - 1);
        mq.client_id[sizeof mq.client_id - 1] = '\0';
    }
    if (json_get_str(body, "base_topic", s, sizeof s) == 0) {
        strncpy(mq.base_topic, s, sizeof mq.base_topic - 1);
        mq.base_topic[sizeof mq.base_topic - 1] = '\0';
    }
    if (json_get_str(body, "home_assistant_prefix", s, sizeof s) == 0) {
        strncpy(mq.home_assistant_prefix, s, sizeof mq.home_assistant_prefix - 1);
        mq.home_assistant_prefix[sizeof mq.home_assistant_prefix - 1] = '\0';
    }

    int v = 0;
    if (json_get_int(body, "port", &v) == 0) {
        if (v < 1 || v > 65535) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "port must be 1..65535");
            return ESP_FAIL;
        }
        mq.port = (uint16_t)v;
    }
    if (json_get_int(body, "keepalive_sec", &v) == 0) {
        if (v < 15 || v > 3600) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "keepalive_sec must be 15..3600");
            return ESP_FAIL;
        }
        mq.keepalive_sec = (uint16_t)v;
    }

    if (mq.enabled && mq.host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled MQTT requires host");
        return ESP_FAIL;
    }
    if (!safe_mqtt_text(mq.host, !mq.enabled, false) ||
        !safe_profile_text(mq.username, true) ||
        !safe_profile_text(mq.password, true) ||
        !safe_mqtt_text(mq.client_id, false, false) ||
        !safe_mqtt_text(mq.base_topic, false, true) ||
        !safe_mqtt_text(mq.home_assistant_prefix, false, true)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad MQTT text");
        return ESP_FAIL;
    }

    esp_err_t err = config_store_save_mqtt(&mq);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    err = mqtt_bridge_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }

    char out[160];
    snprintf(out, sizeof out,
             "{\"saved\":true,\"applied\":true,\"enabled\":%s,\"connected\":%s,\"status\":%u}",
             mq.enabled ? "true" : "false",
             mqtt_bridge_is_connected() ? "true" : "false",
             (unsigned)modbus_store_get(HYDRA_MODBUS_REG_MQTT_STATUS));
    return send_json(req, out);
}

/* ===== Time / sun / schedule config ===== */

static esp_err_t get_time_config(httpd_req_t *req)
{
    config_time_t cfg;
    config_store_load_time(&cfg);
    time_service_status_t st;
    time_service_get_status(&st);
    char buf[512];
    snprintf(buf, sizeof buf,
        "{\"enabled\":%s,"
        "\"server\":\"%s\","
        "\"timezone\":\"%s\","
        "\"synced\":%s,"
        "\"current_epoch\":%lld,"
        "\"current_local\":\"%s\","
        "\"last_sync_epoch\":%lld,"
        "\"last_sync_local\":\"%s\"}",
        cfg.enabled ? "true" : "false",
        cfg.server,
        cfg.timezone,
        st.synced ? "true" : "false",
        (long long)st.current_epoch,
        st.current_local,
        (long long)st.last_sync_epoch,
        st.last_sync_local);
    return send_json(req, buf);
}

static esp_err_t post_time_config(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_time_t cfg;
    config_store_load_time(&cfg);
    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) cfg.enabled = b;
    char s[CONFIG_TZ_LEN];
    if (json_get_str(body, "server", s, sizeof s) == 0) {
        strncpy(cfg.server, s, sizeof cfg.server - 1);
        cfg.server[sizeof cfg.server - 1] = '\0';
    }
    if (json_get_str(body, "timezone", s, sizeof s) == 0) {
        strncpy(cfg.timezone, s, sizeof cfg.timezone - 1);
        cfg.timezone[sizeof cfg.timezone - 1] = '\0';
    }
    if (cfg.enabled && cfg.server[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "enabled time sync requires server");
        return ESP_FAIL;
    }
    if (!safe_mqtt_text(cfg.server, !cfg.enabled, false) ||
        !safe_profile_text(cfg.timezone, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time text");
        return ESP_FAIL;
    }
    if (config_store_save_time(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    esp_err_t err = time_service_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }
    return send_json(req, "{\"saved\":true,\"applied\":true}");
}

static esp_err_t get_sun_config(httpd_req_t *req)
{
    config_sun_t cfg;
    config_store_load_sun(&cfg);
    sun_service_status_t st;
    sun_service_get_status(&st);
    char lat[24];
    char lon[24];
    format_e7(cfg.latitude_e7, lat);
    format_e7(cfg.longitude_e7, lon);
    char buf[640];
    snprintf(buf, sizeof buf,
        "{\"enabled\":%s,"
        "\"location_label\":\"%s\","
        "\"latitude\":%s,"
        "\"longitude\":%s,"
        "\"valid\":%s,"
        "\"sunrise_local\":\"%s\","
        "\"sunset_local\":\"%s\","
        "\"sunrise_minute\":%d,"
        "\"sunset_minute\":%d}",
        cfg.enabled ? "true" : "false",
        cfg.location_label,
        lat,
        lon,
        st.valid ? "true" : "false",
        st.sunrise_local,
        st.sunset_local,
        st.sunrise_minute,
        st.sunset_minute);
    return send_json(req, buf);
}

static esp_err_t post_sun_config(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_sun_t cfg;
    config_store_load_sun(&cfg);
    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) cfg.enabled = b;
    char label[CONFIG_LOCATION_LABEL_LEN];
    if (json_get_str(body, "location_label", label, sizeof label) == 0) {
        strncpy(cfg.location_label, label, sizeof cfg.location_label - 1);
        cfg.location_label[sizeof cfg.location_label - 1] = '\0';
    }
    int32_t e7 = 0;
    if (json_get_double_scaled_e7(body, "latitude", &e7) == 0) cfg.latitude_e7 = e7;
    if (json_get_double_scaled_e7(body, "longitude", &e7) == 0) cfg.longitude_e7 = e7;

    if (!safe_profile_text(cfg.location_label, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad location label");
        return ESP_FAIL;
    }
    if (cfg.latitude_e7 < -900000000 || cfg.latitude_e7 > 900000000 ||
        cfg.longitude_e7 < -1800000000 || cfg.longitude_e7 > 1800000000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad coordinates");
        return ESP_FAIL;
    }
    if (config_store_save_sun(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    esp_err_t err = sun_service_reconfigure();
    if (err == ESP_OK) err = schedule_engine_reconfigure();
    if (err == ESP_OK) err = pump_schedule_engine_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }
    return send_json(req, "{\"saved\":true,\"applied\":true}");
}

static void make_schedule_id(const config_schedules_t *cfg, char out[CONFIG_SCHEDULE_ID_LEN])
{
    for (unsigned n = 1; n < 1000; ++n) {
        char candidate[CONFIG_SCHEDULE_ID_LEN];
        snprintf(candidate, sizeof candidate, "sch-%u", n);
        bool used = false;
        if (cfg) {
            for (uint8_t i = 0; i < cfg->count; ++i) {
                if (strcmp(cfg->schedules[i].schedule_id, candidate) == 0) {
                    used = true;
                    break;
                }
            }
        }
        if (!used) {
            strncpy(out, candidate, CONFIG_SCHEDULE_ID_LEN - 1);
            out[CONFIG_SCHEDULE_ID_LEN - 1] = '\0';
            return;
        }
    }
    strncpy(out, "sch-new", CONFIG_SCHEDULE_ID_LEN - 1);
    out[CONFIG_SCHEDULE_ID_LEN - 1] = '\0';
}

static bool schedule_text_ok(const char *s, bool allow_empty)
{
    return safe_profile_text(s, allow_empty);
}

static esp_err_t get_schedules(httpd_req_t *req)
{
    config_schedules_t cfg;
    config_store_load_schedules(&cfg);
    schedule_engine_status_t st;
    schedule_engine_get_status(&st);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char chunk[768];
    snprintf(chunk, sizeof chunk,
        "{\"running\":%s,\"next_action\":\"%s\",\"schedules\":[",
        st.running ? "true" : "false", st.next_action);
    if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }

    for (uint8_t i = 0; i < cfg.count; ++i) {
        const config_schedule_t *s = &cfg.schedules[i];
        snprintf(chunk, sizeof chunk,
            "%s{\"enabled\":%s,\"schedule_id\":\"%s\",\"name\":\"%s\","
            "\"target_type\":%u,\"target_id\":\"%s\",\"profile_name\":\"%s\","
            "\"intensity_percent\":%u,\"end_intensity_percent\":%u,"
            "\"start_trigger\":%u,\"end_trigger\":%u,"
            "\"start_minute\":%u,\"end_minute\":%u,"
            "\"start_offset_min\":%d,\"end_offset_min\":%d,"
            "\"ramp_up_min\":%u,\"ramp_down_min\":%u}",
            i == 0 ? "" : ",",
            s->enabled ? "true" : "false",
            s->schedule_id,
            s->name,
            (unsigned)s->target_type,
            s->target_id,
            s->profile_name,
            (unsigned)s->intensity_percent,
            (unsigned)s->end_intensity_percent,
            (unsigned)s->start_trigger,
            (unsigned)s->end_trigger,
            (unsigned)s->start_minute,
            (unsigned)s->end_minute,
            (int)s->start_offset_min,
            (int)s->end_offset_min,
            (unsigned)s->ramp_up_min,
            (unsigned)s->ramp_down_min);
        if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    if (httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t post_schedule(httpd_req_t *req)
{
    char body[1536];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_schedules_t cfg;
    config_store_load_schedules(&cfg);

    char id[CONFIG_SCHEDULE_ID_LEN] = {0};
    json_get_str(body, "schedule_id", id, sizeof id);
    int idx = -1;
    if (id[0]) {
        for (uint8_t i = 0; i < cfg.count; ++i) {
            if (strcmp(cfg.schedules[i].schedule_id, id) == 0) {
                idx = (int)i;
                break;
            }
        }
    }
    if (idx < 0) {
        if (cfg.count >= MAX_SCHEDULES) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many schedules");
            return ESP_FAIL;
        }
        idx = cfg.count++;
        memset(&cfg.schedules[idx], 0, sizeof cfg.schedules[idx]);
        if (id[0]) {
            strncpy(cfg.schedules[idx].schedule_id, id, sizeof cfg.schedules[idx].schedule_id - 1);
        } else {
            make_schedule_id(&cfg, cfg.schedules[idx].schedule_id);
        }
        cfg.schedules[idx].enabled = true;
        cfg.schedules[idx].intensity_percent = 70;
        cfg.schedules[idx].end_intensity_percent = 0;
        cfg.schedules[idx].end_minute = 18 * 60;
    }

    config_schedule_t *s = &cfg.schedules[idx];
    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) s->enabled = b;
    char txt[CONFIG_PROFILE_DESC_LEN];
    if (json_get_str(body, "name", txt, sizeof txt) == 0) {
        strncpy(s->name, txt, sizeof s->name - 1);
        s->name[sizeof s->name - 1] = '\0';
    }
    if (json_get_str(body, "target_id", txt, sizeof txt) == 0) {
        strncpy(s->target_id, txt, sizeof s->target_id - 1);
        s->target_id[sizeof s->target_id - 1] = '\0';
    }
    if (json_get_str(body, "profile_name", txt, sizeof txt) == 0) {
        strncpy(s->profile_name, txt, sizeof s->profile_name - 1);
        s->profile_name[sizeof s->profile_name - 1] = '\0';
    }
    int v = 0;
    if (json_get_int(body, "target_type", &v) == 0) s->target_type = (uint8_t)v;
    if (json_get_int(body, "intensity_percent", &v) == 0) s->intensity_percent = (uint8_t)v;
    if (json_get_int(body, "end_intensity_percent", &v) == 0) s->end_intensity_percent = (uint8_t)v;
    if (json_get_int(body, "start_trigger", &v) == 0) s->start_trigger = (uint8_t)v;
    if (json_get_int(body, "end_trigger", &v) == 0) s->end_trigger = (uint8_t)v;
    if (json_get_int(body, "start_minute", &v) == 0) s->start_minute = (uint16_t)v;
    if (json_get_int(body, "end_minute", &v) == 0) s->end_minute = (uint16_t)v;
    if (json_get_int(body, "start_offset_min", &v) == 0) s->start_offset_min = (int16_t)v;
    if (json_get_int(body, "end_offset_min", &v) == 0) s->end_offset_min = (int16_t)v;
    if (json_get_int(body, "ramp_up_min", &v) == 0) s->ramp_up_min = (uint16_t)v;
    if (json_get_int(body, "ramp_down_min", &v) == 0) s->ramp_down_min = (uint16_t)v;

    if (!s->name[0]) strncpy(s->name, "Schedule", sizeof s->name - 1);
    if (!schedule_text_ok(s->schedule_id, false) ||
        !schedule_text_ok(s->name, false) ||
        !schedule_text_ok(s->target_id, false) ||
        !schedule_text_ok(s->profile_name, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad schedule text");
        return ESP_FAIL;
    }
    if (s->target_type > CONFIG_SCHEDULE_TARGET_GROUP ||
        s->intensity_percent > 100 || s->end_intensity_percent > 100 ||
        s->start_trigger > CONFIG_SCHEDULE_TRIGGER_SUNSET ||
        s->end_trigger > CONFIG_SCHEDULE_TRIGGER_SUNSET ||
        s->start_minute > 1439 || s->end_minute > 1439 ||
        s->start_offset_min < -720 || s->start_offset_min > 720 ||
        s->end_offset_min < -720 || s->end_offset_min > 720 ||
        s->ramp_up_min > 1440 || s->ramp_down_min > 1440) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "schedule value out of range");
        return ESP_FAIL;
    }

    if (config_store_save_schedules(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    esp_err_t err = schedule_engine_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }
    char out[128];
    snprintf(out, sizeof out, "{\"saved\":true,\"schedule_id\":\"%s\"}", s->schedule_id);
    return send_json(req, out);
}

static esp_err_t delete_schedule(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/schedules/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/schedules/");
    char id[CONFIG_SCHEDULE_ID_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < sizeof id) { id[i] = p[i]; ++i; }
    id[i] = '\0';
    if (!id[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing schedule_id"); return ESP_FAIL; }

    config_schedules_t cfg;
    config_store_load_schedules(&cfg);
    int found = -1;
    for (uint8_t k = 0; k < cfg.count; ++k) {
        if (strcmp(cfg.schedules[k].schedule_id, id) == 0) { found = (int)k; break; }
    }
    if (found < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "schedule not found");
        return ESP_FAIL;
    }
    for (uint8_t k = (uint8_t)found; k + 1 < cfg.count; ++k) {
        cfg.schedules[k] = cfg.schedules[k + 1];
    }
    --cfg.count;
    memset(&cfg.schedules[cfg.count], 0, sizeof cfg.schedules[cfg.count]);
    if (config_store_save_schedules(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    schedule_engine_reconfigure();
    char out[96];
    snprintf(out, sizeof out, "{\"deleted\":true,\"schedule_id\":\"%s\"}", id);
    return send_json(req, out);
}

/* ===== pump schedules ===== */

static void make_pump_schedule_id(const config_pump_schedules_t *cfg, char out[CONFIG_SCHEDULE_ID_LEN])
{
    for (unsigned n = 1; n < 1000; ++n) {
        char candidate[CONFIG_SCHEDULE_ID_LEN];
        snprintf(candidate, sizeof candidate, "psch-%u", n);
        bool used = false;
        if (cfg) {
            for (uint8_t i = 0; i < cfg->count; ++i) {
                if (strcmp(cfg->schedules[i].schedule_id, candidate) == 0) {
                    used = true;
                    break;
                }
            }
        }
        if (!used) {
            strncpy(out, candidate, CONFIG_SCHEDULE_ID_LEN - 1);
            out[CONFIG_SCHEDULE_ID_LEN - 1] = '\0';
            return;
        }
    }
    strncpy(out, "psch-new", CONFIG_SCHEDULE_ID_LEN - 1);
    out[CONFIG_SCHEDULE_ID_LEN - 1] = '\0';
}

static esp_err_t get_pump_schedules(httpd_req_t *req)
{
    config_pump_schedules_t cfg;
    config_store_load_pump_schedules(&cfg);
    pump_schedule_engine_status_t st;
    pump_schedule_engine_get_status(&st);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char chunk[768];
    snprintf(chunk, sizeof chunk,
        "{\"running\":%s,\"next_action\":\"%s\",\"schedules\":[",
        st.running ? "true" : "false", st.next_action);
    if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;

    for (uint8_t i = 0; i < cfg.count; ++i) {
        const config_pump_schedule_t *s = &cfg.schedules[i];
        int n = snprintf(chunk, sizeof chunk,
            "%s{\"enabled\":%s,\"schedule_id\":\"%s\",\"name\":\"%s\","
            "\"target_id\":\"%s\",\"active_mode\":%u,\"active_speed_percent\":%u,"
            "\"active_min_speed_percent\":%u,\"active_variance_percent\":%u,"
            "\"active_on_time_ms\":%u,\"active_off_time_ms\":%u,"
            "\"end_mode\":%u,\"end_speed_percent\":%u,"
            "\"end_min_speed_percent\":%u,\"end_variance_percent\":%u,"
            "\"end_on_time_ms\":%u,\"end_off_time_ms\":%u,"
            "\"start_trigger\":%u,\"end_trigger\":%u,"
            "\"start_minute\":%u,\"end_minute\":%u,"
            "\"start_offset_min\":%d,\"end_offset_min\":%d}",
            i == 0 ? "" : ",",
            s->enabled ? "true" : "false",
            s->schedule_id,
            s->name,
            s->target_id,
            (unsigned)s->active_mode,
            (unsigned)s->active_speed_percent,
            (unsigned)s->active_min_speed_percent,
            (unsigned)s->active_variance_percent,
            (unsigned)s->active_on_time_ms,
            (unsigned)s->active_off_time_ms,
            (unsigned)s->end_mode,
            (unsigned)s->end_speed_percent,
            (unsigned)s->end_min_speed_percent,
            (unsigned)s->end_variance_percent,
            (unsigned)s->end_on_time_ms,
            (unsigned)s->end_off_time_ms,
            (unsigned)s->start_trigger,
            (unsigned)s->end_trigger,
            (unsigned)s->start_minute,
            (unsigned)s->end_minute,
            (int)s->start_offset_min,
            (int)s->end_offset_min);
        if (n < 0 || (size_t)n >= sizeof chunk) return ESP_FAIL;
        if (httpd_resp_send_chunk(req, chunk, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    }
    if (httpd_resp_send_chunk(req, "]}", HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t post_pump_schedule(httpd_req_t *req)
{
    char body[1024];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_pump_schedules_t cfg;
    config_store_load_pump_schedules(&cfg);

    char id[CONFIG_SCHEDULE_ID_LEN] = {0};
    json_get_str(body, "schedule_id", id, sizeof id);
    int idx = -1;
    if (id[0]) {
        for (uint8_t i = 0; i < cfg.count; ++i) {
            if (strcmp(cfg.schedules[i].schedule_id, id) == 0) {
                idx = (int)i;
                break;
            }
        }
    }
    if (idx < 0) {
        if (cfg.count >= MAX_PUMP_SCHEDULES) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many pump schedules");
            return ESP_FAIL;
        }
        idx = cfg.count++;
        memset(&cfg.schedules[idx], 0, sizeof cfg.schedules[idx]);
        if (id[0]) {
            strncpy(cfg.schedules[idx].schedule_id, id, sizeof cfg.schedules[idx].schedule_id - 1);
        } else {
            make_pump_schedule_id(&cfg, cfg.schedules[idx].schedule_id);
        }
        cfg.schedules[idx].enabled = true;
        cfg.schedules[idx].active_mode = CONFIG_PUMP_MODE_CONSTANT;
        cfg.schedules[idx].active_speed_percent = 40;
        cfg.schedules[idx].active_min_speed_percent = 20;
        cfg.schedules[idx].active_variance_percent = 50;
        cfg.schedules[idx].active_on_time_ms = 1000;
        cfg.schedules[idx].active_off_time_ms = 1000;
        cfg.schedules[idx].end_mode = CONFIG_PUMP_MODE_CONSTANT;
        cfg.schedules[idx].end_speed_percent = 20;
        cfg.schedules[idx].end_min_speed_percent = 10;
        cfg.schedules[idx].end_variance_percent = 50;
        cfg.schedules[idx].end_on_time_ms = 1000;
        cfg.schedules[idx].end_off_time_ms = 1000;
        cfg.schedules[idx].end_minute = 18 * 60;
    }

    config_pump_schedule_t *s = &cfg.schedules[idx];
    bool b = false;
    if (json_get_bool(body, "enabled", &b) == 0) s->enabled = b;
    char txt[CONFIG_PROFILE_DESC_LEN];
    if (json_get_str(body, "name", txt, sizeof txt) == 0) {
        strncpy(s->name, txt, sizeof s->name - 1);
        s->name[sizeof s->name - 1] = '\0';
    }
    if (json_get_str(body, "target_id", txt, sizeof txt) == 0) {
        strncpy(s->target_id, txt, sizeof s->target_id - 1);
        s->target_id[sizeof s->target_id - 1] = '\0';
    }
    int v = 0;
    if (json_get_int(body, "active_mode", &v) == 0) s->active_mode = (uint8_t)v;
    if (json_get_int(body, "active_speed_percent", &v) == 0) s->active_speed_percent = (uint8_t)v;
    if (json_get_int(body, "active_min_speed_percent", &v) == 0) s->active_min_speed_percent = (uint8_t)v;
    if (json_get_int(body, "active_variance_percent", &v) == 0) s->active_variance_percent = (uint8_t)v;
    if (json_get_int(body, "active_on_time_ms", &v) == 0) s->active_on_time_ms = (uint16_t)v;
    if (json_get_int(body, "active_off_time_ms", &v) == 0) s->active_off_time_ms = (uint16_t)v;
    if (json_get_int(body, "end_mode", &v) == 0) s->end_mode = (uint8_t)v;
    if (json_get_int(body, "end_speed_percent", &v) == 0) s->end_speed_percent = (uint8_t)v;
    if (json_get_int(body, "end_min_speed_percent", &v) == 0) s->end_min_speed_percent = (uint8_t)v;
    if (json_get_int(body, "end_variance_percent", &v) == 0) s->end_variance_percent = (uint8_t)v;
    if (json_get_int(body, "end_on_time_ms", &v) == 0) s->end_on_time_ms = (uint16_t)v;
    if (json_get_int(body, "end_off_time_ms", &v) == 0) s->end_off_time_ms = (uint16_t)v;
    if (json_get_int(body, "start_trigger", &v) == 0) s->start_trigger = (uint8_t)v;
    if (json_get_int(body, "end_trigger", &v) == 0) s->end_trigger = (uint8_t)v;
    if (json_get_int(body, "start_minute", &v) == 0) s->start_minute = (uint16_t)v;
    if (json_get_int(body, "end_minute", &v) == 0) s->end_minute = (uint16_t)v;
    if (json_get_int(body, "start_offset_min", &v) == 0) s->start_offset_min = (int16_t)v;
    if (json_get_int(body, "end_offset_min", &v) == 0) s->end_offset_min = (int16_t)v;

    if (!s->name[0]) strncpy(s->name, "Pump Schedule", sizeof s->name - 1);
    if (!schedule_text_ok(s->schedule_id, false) ||
        !schedule_text_ok(s->name, false) ||
        !schedule_text_ok(s->target_id, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pump schedule text");
        return ESP_FAIL;
    }
    if (!pump_mode_ui_supported(s->active_mode) ||
        !pump_mode_ui_supported(s->end_mode) ||
        s->active_speed_percent > 100 || s->end_speed_percent > 100 ||
        s->active_min_speed_percent > 100 || s->end_min_speed_percent > 100 ||
        s->active_variance_percent > 100 || s->end_variance_percent > 100 ||
        s->active_on_time_ms == 0 || s->active_on_time_ms > 60000 ||
        s->active_off_time_ms == 0 || s->active_off_time_ms > 60000 ||
        s->end_on_time_ms == 0 || s->end_on_time_ms > 60000 ||
        s->end_off_time_ms == 0 || s->end_off_time_ms > 60000 ||
        s->start_trigger > CONFIG_SCHEDULE_TRIGGER_SUNSET ||
        s->end_trigger > CONFIG_SCHEDULE_TRIGGER_SUNSET ||
        s->start_minute > 1439 || s->end_minute > 1439 ||
        s->start_offset_min < -720 || s->start_offset_min > 720 ||
        s->end_offset_min < -720 || s->end_offset_min > 720) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pump schedule value out of range");
        return ESP_FAIL;
    }
    if (!pump_registry_get(s->target_id)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "pump target not found");
        return ESP_FAIL;
    }

    if (config_store_save_pump_schedules(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    esp_err_t err = pump_schedule_engine_reconfigure();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "reconfigure failed");
        return ESP_FAIL;
    }
    char out[128];
    snprintf(out, sizeof out, "{\"saved\":true,\"schedule_id\":\"%s\"}", s->schedule_id);
    return send_json(req, out);
}

static esp_err_t delete_pump_schedule(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/pump-schedules/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/pump-schedules/");
    char id[CONFIG_SCHEDULE_ID_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < sizeof id) { id[i] = p[i]; ++i; }
    id[i] = '\0';
    if (!id[0]) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing schedule_id"); return ESP_FAIL; }

    config_pump_schedules_t cfg;
    config_store_load_pump_schedules(&cfg);
    int found = -1;
    for (uint8_t k = 0; k < cfg.count; ++k) {
        if (strcmp(cfg.schedules[k].schedule_id, id) == 0) { found = (int)k; break; }
    }
    if (found < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "pump schedule not found");
        return ESP_FAIL;
    }
    for (uint8_t k = (uint8_t)found; k + 1 < cfg.count; ++k) {
        cfg.schedules[k] = cfg.schedules[k + 1];
    }
    --cfg.count;
    memset(&cfg.schedules[cfg.count], 0, sizeof cfg.schedules[cfg.count]);
    if (config_store_save_pump_schedules(&cfg) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    pump_schedule_engine_reconfigure();
    char out[96];
    snprintf(out, sizeof out, "{\"deleted\":true,\"schedule_id\":\"%s\"}", id);
    return send_json(req, out);
}

/* ===== profiles (named channel intensity mixes) ===== */

static esp_err_t get_profiles(httpd_req_t *req)
{
    config_profiles_t p;
    if (config_store_load_profiles(&p) != ESP_OK) {
        p.count = 0;
    }
    char buf[2048];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"profiles\":[");
    bool first = true;
    for (size_t i = 0; i < k_builtin_profile_count && off < sizeof buf - 350; ++i) {
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"name\":\"%s\",\"description\":\"%s\",\"builtin\":true,\"intensities\":[",
            first ? "" : ",", k_builtin_profiles[i].name,
            k_builtin_profiles[i].description);
        for (int j = 0; j < 9; j++) {
            off += snprintf(buf + off, sizeof buf - off,
                "%s%u", j==0 ? "" : ",", (unsigned)k_builtin_profiles[i].intensities[j]);
        }
        off += snprintf(buf + off, sizeof buf - off, "]}");
        first = false;
    }
    for (size_t i = 0; i < p.count && off < sizeof buf - 350; ++i) {
        if (builtin_profile_by_name(p.profiles[i].name)) continue;
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"name\":\"%s\",\"description\":\"%s\",\"builtin\":false,\"intensities\":[",
            first ? "" : ",", p.profiles[i].name,
            p.profiles[i].description);
        for (int j = 0; j < 9; j++) {
            off += snprintf(buf + off, sizeof buf - off,
                "%s%u", j==0 ? "" : ",", (unsigned)p.profiles[i].intensities[j]);
        }
        off += snprintf(buf + off, sizeof buf - off, "]}");
        first = false;
    }
    off += snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

static esp_err_t post_profile(httpd_req_t *req)
{
    char body[768];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    config_profiles_t p;
    if (config_store_load_profiles(&p) != ESP_OK) {
        p.count = 0;
    }

    char name[CONFIG_PROFILE_NAME_LEN];
    if (json_get_str(body, "name", name, sizeof name) != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name");
        return ESP_FAIL;
    }
    if (!safe_profile_text(name, false)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    if (builtin_profile_by_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "built-in profile name is reserved");
        return ESP_FAIL;
    }
    char description[CONFIG_PROFILE_DESC_LEN] = {0};
    if (json_get_str(body, "description", description, sizeof description) != 0) {
        description[0] = '\0';
    }
    if (!safe_profile_text(description, true)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad description");
        return ESP_FAIL;
    }

    int idx = -1;
    for (size_t i = 0; i < p.count; ++i) {
        if (strcmp(p.profiles[i].name, name) == 0) {
            idx = (int)i; break;
        }
    }
    if (idx < 0) {
        if (p.count >= MAX_USER_PROFILES) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too many profiles");
            return ESP_FAIL;
        }
        idx = p.count;
        strncpy(p.profiles[idx].name, name, sizeof p.profiles[idx].name - 1);
        p.profiles[idx].name[sizeof p.profiles[idx].name - 1] = '\0';
        p.profiles[idx].description[0] = '\0';
        memset(p.profiles[idx].intensities, 0, sizeof p.profiles[idx].intensities);
        p.count++;
    }
    strncpy(p.profiles[idx].description, description,
            sizeof p.profiles[idx].description - 1);
    p.profiles[idx].description[sizeof p.profiles[idx].description - 1] = '\0';

    for (int j = 0; j < 9; j++) {
        int v;
        if (json_get_int(body, channel_names[j], &v) == 0) {
            if (v < 0) v = 0;
            if (v > 1000) v = 1000;
            p.profiles[idx].intensities[j] = (uint16_t)v;
        }
    }

    if (config_store_save_profiles(&p) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    char out[320];
    snprintf(out, sizeof out, "{\"saved\":true,\"name\":\"%s\",\"description\":\"%s\"}",
             name, p.profiles[idx].description);
    return send_json(req, out);
}

static esp_err_t delete_profile(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/profiles/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/profiles/");
    char name[CONFIG_PROFILE_NAME_LEN];
    size_t i = 0;
    while (p[i] && p[i] != '/' && i + 1 < sizeof name) { name[i] = p[i]; ++i; }
    name[i] = '\0';
    if (i == 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing name"); return ESP_FAIL; }
    if (builtin_profile_by_name(name)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "built-in profile cannot be deleted");
        return ESP_FAIL;
    }

    config_profiles_t profs;
    if (config_store_load_profiles(&profs) != ESP_OK) profs.count = 0;

    int found = -1;
    for (size_t k = 0; k < profs.count; ++k) {
        if (strcmp(profs.profiles[k].name, name) == 0) { found = (int)k; break; }
    }
    if (found < 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_FAIL;
    }
    for (size_t k = found; k < profs.count - 1; ++k) {
        profs.profiles[k] = profs.profiles[k + 1];
    }
    profs.count--;
    if (config_store_save_profiles(&profs) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    char out[96];
    snprintf(out, sizeof out, "{\"deleted\":true,\"name\":\"%s\"}", name);
    return send_json(req, out);
}

static esp_err_t submit_web_command(httpd_req_t *req,
                                    const char *target_id,
                                    ce_target_type_t target_type,
                                    const char *body)
{
    ce_request_t r;
    memset(&r, 0, sizeof r);
    strncpy(r.target_id, target_id, LIGHT_ID_LEN - 1);
    r.target_type = target_type;
    r.scene_timeout_sec = 60;
    r.command_timeout_ms = 30000;
    r.replace = true;

    bool special = false;
    char prof_name[CONFIG_PROFILE_NAME_LEN] = {0};
    if (json_get_str(body, "profile", prof_name, sizeof prof_name) == 0) {
        const user_profile_t *builtin = builtin_profile_by_name(prof_name);
        if (builtin) {
            r.kind = CE_KIND_SET_CHANNELS;
            r.channel_count = 9;
            for (int jj = 0; jj < 9; ++jj) {
                r.channels[jj].name = channel_names[jj];
                r.channels[jj].value = builtin->intensities[jj];
            }
            special = true;
        }
        config_profiles_t ps;
        if (!special && config_store_load_profiles(&ps) == ESP_OK) {
            for (uint8_t ii = 0; ii < ps.count; ++ii) {
                if (strcmp(ps.profiles[ii].name, prof_name) == 0) {
                    r.kind = CE_KIND_SET_CHANNELS;
                    r.channel_count = 9;
                    for (int jj = 0; jj < 9; ++jj) {
                        r.channels[jj].name = channel_names[jj];
                        r.channels[jj].value = ps.profiles[ii].intensities[jj];
                    }
                    special = true;
                    break;
                }
            }
        }
    }
    int idelta = 0;
    if (!special && json_get_int(body, "intensity_delta", &idelta) == 0) {
        r.kind = CE_KIND_INTENSITY_ADJUST;
        r.intensity_delta = (int16_t)idelta;
        special = true;
    }
    if (!special) {
        if (target_type != CE_TARGET_LIGHT || mqtt_parse_light_command(body, target_id, &r) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid command");
            return ESP_FAIL;
        }
        r.target_type = CE_TARGET_LIGHT;
    }
    r.source = CMD_SOURCE_WEB;

    char cmd_id[CMD_ID_LEN];
    ce_result_t res = command_engine_submit(&r, cmd_id);

    char out[128];
    snprintf(out, sizeof out,
             "{\"command_id\":\"%s\",\"result\":%d}", cmd_id, (int)res);
    return send_json(req, out);
}

static esp_err_t post_light_command(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/lights/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/lights/");
    const char *slash = strchr(p, '/');
    if (!slash) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing light_id"); return ESP_FAIL; }
    char light_id[LIGHT_ID_LEN];
    size_t id_len = slash - p;
    if (id_len >= LIGHT_ID_LEN) id_len = LIGHT_ID_LEN - 1;
    memcpy(light_id, p, id_len);
    light_id[id_len] = '\0';

    char body[1024];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    if (strcmp(slash, "/rename") == 0) {
        char display_name[LIGHT_NAME_LEN];
        if (json_get_str(body, "display_name", display_name, sizeof display_name) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing display_name");
            return ESP_FAIL;
        }
        if (!safe_display_name(display_name)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad display_name");
            return ESP_FAIL;
        }
        if (light_registry_rename(light_id, display_name) != 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "rename failed");
            return ESP_FAIL;
        }
        light_registry_save();
        const registered_light_t *light = light_registry_get(light_id);
        char out[128];
        snprintf(out, sizeof out,
                 "{\"renamed\":true,\"light_id\":\"%s\",\"display_name\":\"%s\"}",
                 light_id, light ? light->display_name : display_name);
        return send_json(req, out);
    }

    if (strcmp(slash, "/command") != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown light subpath");
        return ESP_FAIL;
    }

    return submit_web_command(req, light_id, CE_TARGET_LIGHT, body);
}

static esp_err_t post_group_command(httpd_req_t *req)
{
    const char *p = strstr(req->uri, "/api/groups/");
    if (!p) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad uri"); return ESP_FAIL; }
    p += strlen("/api/groups/");
    const char *slash = strchr(p, '/');
    if (!slash) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing group_id"); return ESP_FAIL; }
    char group_id[GROUP_ID_LEN];
    size_t id_len = slash - p;
    if (id_len >= GROUP_ID_LEN) id_len = GROUP_ID_LEN - 1;
    memcpy(group_id, p, id_len);
    group_id[id_len] = '\0';
    if (strcmp(slash, "/command") != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "unknown group subpath");
        return ESP_FAIL;
    }

    char body[1024];
    int total = 0;
    while (total < (int)sizeof body - 1) {
        int got = httpd_req_recv(req, body + total, sizeof body - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total < 0 ? 0 : total] = '\0';

    return submit_web_command(req, group_id, CE_TARGET_GROUP, body);
}

static esp_err_t get_logs(httpd_req_t *req)
{
    char buf[4096];
    size_t off = 0;
    off += snprintf(buf + off, sizeof buf - off, "{\"events\":[");
    size_t n = event_log_count();
    bool first = true;
    for (size_t i = 0; i < n && off < sizeof buf - 200; ++i) {
        event_log_entry_t e;
        if (!event_log_get(i, &e)) continue;
        off += snprintf(buf + off, sizeof buf - off,
            "%s{\"uptime_ms\":%llu,\"level\":%d,\"code\":\"%s\","
            "\"light_id\":\"%s\",\"command_id\":\"%s\","
            "\"message\":\"%s\"}",
            first ? "" : ",",
            (unsigned long long)e.uptime_ms,
            (int)e.level, e.code, e.light_id, e.command_id, e.message);
        first = false;
    }
    snprintf(buf + off, sizeof buf - off, "]}");
    return send_json(req, buf);
}

static esp_err_t post_ota(httpd_req_t *req)
{
    esp_err_t err = ota_update_begin();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_begin");
        return ESP_FAIL;
    }
    char buf[1024];
    int received = 0;
    while (1) {
        int got = httpd_req_recv(req, buf, sizeof buf);
        if (got <= 0) break;
        err = ota_update_write(buf, got);
        if (err != ESP_OK) {
            ota_update_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_write");
            return ESP_FAIL;
        }
        received += got;
    }
    err = ota_update_end();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota_end");
        return ESP_FAIL;
    }
    char out[128];
    snprintf(out, sizeof out, "{\"received\":%d,\"status\":\"success\"}", received);
    send_json(req, out);
    return ESP_OK;
}

esp_err_t web_ui_init(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 42;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: 0x%x", err);
        return err;
    }

    static const httpd_uri_t r_root          = { .uri = "/",                     .method = HTTP_GET,  .handler = get_root };
    static const httpd_uri_t r_root_head     = { .uri = "/",                     .method = HTTP_HEAD, .handler = head_root };
    static const httpd_uri_t r_favicon       = { .uri = "/favicon.ico",          .method = HTTP_GET,  .handler = get_favicon };
    static const httpd_uri_t r_status        = { .uri = "/api/status",           .method = HTTP_GET,  .handler = get_status };
    static const httpd_uri_t r_scan          = { .uri = "/api/scan",             .method = HTTP_POST, .handler = post_scan };
    static const httpd_uri_t r_scan_results  = { .uri = "/api/scan/results",     .method = HTTP_GET,  .handler = get_scan_results };
    static const httpd_uri_t r_lights_get    = { .uri = "/api/lights",           .method = HTTP_GET,  .handler = get_lights };
    static const httpd_uri_t r_lights_post   = { .uri = "/api/lights",           .method = HTTP_POST, .handler = post_lights };
    static const httpd_uri_t r_light_cmd     = { .uri = "/api/lights/*",         .method = HTTP_POST, .handler = post_light_command };
    static const httpd_uri_t r_light_del     = { .uri = "/api/lights/*",         .method = HTTP_DELETE, .handler = delete_light };
    static const httpd_uri_t r_pumps_get     = { .uri = "/api/pumps",            .method = HTTP_GET,  .handler = get_pumps };
    static const httpd_uri_t r_pumps_post    = { .uri = "/api/pumps",            .method = HTTP_POST, .handler = post_pumps };
    static const httpd_uri_t r_pump_cmd      = { .uri = "/api/pumps/*",          .method = HTTP_POST, .handler = post_pump_command };
    static const httpd_uri_t r_pump_del      = { .uri = "/api/pumps/*",          .method = HTTP_DELETE, .handler = delete_pump };
    static const httpd_uri_t r_groups_get    = { .uri = "/api/groups",           .method = HTTP_GET,  .handler = get_groups };
    static const httpd_uri_t r_groups_post   = { .uri = "/api/groups",           .method = HTTP_POST, .handler = post_groups };
    static const httpd_uri_t r_group_cmd     = { .uri = "/api/groups/*",         .method = HTTP_POST, .handler = post_group_command };
    static const httpd_uri_t r_group_del     = { .uri = "/api/groups/*",         .method = HTTP_DELETE, .handler = delete_group };
    static const httpd_uri_t r_logs          = { .uri = "/api/logs",             .method = HTTP_GET,  .handler = get_logs };
    static const httpd_uri_t r_ota           = { .uri = "/api/ota",              .method = HTTP_POST, .handler = post_ota };
    static const httpd_uri_t r_wifi_get      = { .uri = "/api/config/wifi",      .method = HTTP_GET,  .handler = get_wifi_config };
    static const httpd_uri_t r_wifi_post     = { .uri = "/api/config/wifi",      .method = HTTP_POST, .handler = post_wifi_config };
    static const httpd_uri_t r_modbus_get    = { .uri = "/api/config/modbus",    .method = HTTP_GET,  .handler = get_modbus_config };
    static const httpd_uri_t r_modbus_post   = { .uri = "/api/config/modbus",    .method = HTTP_POST, .handler = post_modbus_config };
    static const httpd_uri_t r_mqtt_get      = { .uri = "/api/config/mqtt",      .method = HTTP_GET,  .handler = get_mqtt_config };
    static const httpd_uri_t r_mqtt_post     = { .uri = "/api/config/mqtt",      .method = HTTP_POST, .handler = post_mqtt_config };
    static const httpd_uri_t r_time_get      = { .uri = "/api/config/time",      .method = HTTP_GET,  .handler = get_time_config };
    static const httpd_uri_t r_time_post     = { .uri = "/api/config/time",      .method = HTTP_POST, .handler = post_time_config };
    static const httpd_uri_t r_sun_get       = { .uri = "/api/config/sun",       .method = HTTP_GET,  .handler = get_sun_config };
    static const httpd_uri_t r_sun_post      = { .uri = "/api/config/sun",       .method = HTTP_POST, .handler = post_sun_config };
    static const httpd_uri_t r_sched_get     = { .uri = "/api/schedules",        .method = HTTP_GET,  .handler = get_schedules };
    static const httpd_uri_t r_sched_post    = { .uri = "/api/schedules",        .method = HTTP_POST, .handler = post_schedule };
    static const httpd_uri_t r_sched_del     = { .uri = "/api/schedules/*",      .method = HTTP_DELETE, .handler = delete_schedule };
    static const httpd_uri_t r_pump_sched_get  = { .uri = "/api/pump-schedules",   .method = HTTP_GET,  .handler = get_pump_schedules };
    static const httpd_uri_t r_pump_sched_post = { .uri = "/api/pump-schedules",   .method = HTTP_POST, .handler = post_pump_schedule };
    static const httpd_uri_t r_pump_sched_del  = { .uri = "/api/pump-schedules/*", .method = HTTP_DELETE, .handler = delete_pump_schedule };
    static const httpd_uri_t r_profiles_get  = { .uri = "/api/profiles",         .method = HTTP_GET,  .handler = get_profiles };
    static const httpd_uri_t r_profiles_post = { .uri = "/api/profiles",         .method = HTTP_POST, .handler = post_profile };
    static const httpd_uri_t r_profile_del   = { .uri = "/api/profiles/*",       .method = HTTP_DELETE, .handler = delete_profile };

    httpd_register_uri_handler(s_server, &r_root);
    httpd_register_uri_handler(s_server, &r_root_head);
    httpd_register_uri_handler(s_server, &r_favicon);
    httpd_register_uri_handler(s_server, &r_status);
    httpd_register_uri_handler(s_server, &r_scan);
    httpd_register_uri_handler(s_server, &r_scan_results);
    httpd_register_uri_handler(s_server, &r_lights_get);
    httpd_register_uri_handler(s_server, &r_lights_post);
    httpd_register_uri_handler(s_server, &r_light_cmd);
    httpd_register_uri_handler(s_server, &r_light_del);
    httpd_register_uri_handler(s_server, &r_pumps_get);
    httpd_register_uri_handler(s_server, &r_pumps_post);
    httpd_register_uri_handler(s_server, &r_pump_cmd);
    httpd_register_uri_handler(s_server, &r_pump_del);
    httpd_register_uri_handler(s_server, &r_groups_get);
    httpd_register_uri_handler(s_server, &r_groups_post);
    httpd_register_uri_handler(s_server, &r_group_cmd);
    httpd_register_uri_handler(s_server, &r_group_del);
    httpd_register_uri_handler(s_server, &r_logs);
    httpd_register_uri_handler(s_server, &r_ota);
    httpd_register_uri_handler(s_server, &r_wifi_get);
    httpd_register_uri_handler(s_server, &r_wifi_post);
    httpd_register_uri_handler(s_server, &r_modbus_get);
    httpd_register_uri_handler(s_server, &r_modbus_post);
    httpd_register_uri_handler(s_server, &r_mqtt_get);
    httpd_register_uri_handler(s_server, &r_mqtt_post);
    httpd_register_uri_handler(s_server, &r_time_get);
    httpd_register_uri_handler(s_server, &r_time_post);
    httpd_register_uri_handler(s_server, &r_sun_get);
    httpd_register_uri_handler(s_server, &r_sun_post);
    httpd_register_uri_handler(s_server, &r_sched_get);
    httpd_register_uri_handler(s_server, &r_sched_post);
    httpd_register_uri_handler(s_server, &r_sched_del);
    httpd_register_uri_handler(s_server, &r_pump_sched_get);
    httpd_register_uri_handler(s_server, &r_pump_sched_post);
    httpd_register_uri_handler(s_server, &r_pump_sched_del);
    httpd_register_uri_handler(s_server, &r_profiles_get);
    httpd_register_uri_handler(s_server, &r_profiles_post);
    httpd_register_uri_handler(s_server, &r_profile_del);

    return ESP_OK;
}

#endif /* ESP_PLATFORM */
