// The first-run wizard: seven steps from "what this is" to the models downloaded, which set up
// built-in speech and cleanup and leave every other choice for the settings window. Nothing is
// saved until Finish. Ported from renderer/wizard.js and wizard.html. Everything here runs on the
// main thread, with the loop's lock held for the length of a frame.
#include "wizard_window.h"
#include <math.h>
#include "catalog.h"
#include "commit.h"
#include "defaults.h"
#include "activate.h"
#include "gs_desk.h"
#include "fonts.h"
#include "gs_draw.h"
#include "hotkeys.h"
#include "loop.h"
#include "model_jobs.h"
#include "settings.h"
#include "settings_window.h"
#include "ui.h"

enum { STEP_WELCOME, STEP_HOTKEY, STEP_MIC, STEP_STT, STEP_CLEANUP, STEP_OUTPUT, STEP_FINISH, STEPS };

typedef struct { const eh_model *model; bool skipped; } download_row;

static struct {
    eh_ui *ui;
    int step;
    gs_jval *base;
    eh_form_baseline baseline;
    char hotkey[64], hotkey_status[400];
    char **mics;
    int mic_count, mic;
    bool no_mics;
    bool cleanup_enabled;
    int cleanup_model, style, output_mode;
    // The last step: what is needed, and whether its downloads were begun for this set of models.
    download_row rows[2];
    int row_count;
    bool started;
    char finish_status[320];
    bool finish_failed, saving;
    int outcome;  // set by the save: 1 saved, 2 the hotkey was refused
} Z;

static int cleanup_models(const eh_model **out, int max) { return eh_models_list(EH_MODEL_CLEANUP, out, max); }

static const eh_model *cleanup_model(void) {
    const eh_model *list[32];
    int n = cleanup_models(list, 32);
    return Z.cleanup_model >= 0 && Z.cleanup_model < n ? list[Z.cleanup_model] : NULL;
}

static const char *mode_id(int mode) { return mode == 2 ? "clipboard" : mode == 1 ? "paste-copy" : "paste"; }

static void load(void) {
    const gs_jval *c = Z.base;
    SDL_strlcpy(Z.hotkey, gs_jval_string(gs_jval_get(c, "hotkey"), ""), sizeof Z.hotkey);
    Z.cleanup_enabled = gs_jval_boolean(gs_jval_path(c, "cleanup.enabled"), true);
    const eh_model *list[32];
    int n = cleanup_models(list, 32);
    Z.cleanup_model = 0;
    const char *chosen = gs_jval_string(gs_jval_path(c, "cleanup.builtin.model"), "");
    for (int i = 0; i < n; i++)
        if (!SDL_strcmp(list[i]->id, chosen) || (!*chosen && list[i]->is_default)) Z.cleanup_model = i;
    Z.style = 0;
    const char *style = gs_jval_string(gs_jval_path(c, "cleanup.style"), EH_DEFAULT_STYLE);
    for (size_t i = 0; i < SDL_arraysize(eh_styles); i++)
        if (!SDL_strcmp(eh_styles[i].id, !SDL_strcmp(style, "custom") ? EH_DEFAULT_STYLE : style)) Z.style = (int)i;
    const char *mode = eh_displayed_output_mode(c);
    Z.output_mode = !SDL_strcmp(mode, "clipboard") ? 2 : !SDL_strcmp(mode, "paste-copy") ? 1 : 0;
    eh_form_baseline_take(&Z.baseline, c);
    // The microphones.
    for (int i = 0; i < Z.mic_count; i++) SDL_free(Z.mics[i]);
    SDL_free(Z.mics);
    int count = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&count);
    const char *saved = gs_jval_string(gs_jval_path(c, "audio.deviceId"), "");
    Z.mics = SDL_calloc((size_t)count + 2, sizeof *Z.mics);
    Z.mic_count = 0;
    Z.mics[Z.mic_count++] = SDL_strdup("System default");
    Z.mic = 0;
    for (int i = 0; ids && i < count; i++) {
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        if (!name) continue;
        if (!SDL_strcmp(name, saved)) Z.mic = Z.mic_count;
        Z.mics[Z.mic_count++] = SDL_strdup(name);
    }
    Z.no_mics = !ids || !count;
    SDL_free(ids);
}

// The wizard's choices as settings: built-in speech and cleanup, and what the steps asked.
static gs_jval *collect(void) {
    gs_jval *n = gs_jval_clone(Z.base);
    gs_jval_set(n, "hotkey", gs_jval_str(Z.hotkey));
    gs_jval_set_path(n, "output.mode", gs_jval_str(mode_id(Z.output_mode)));
    gs_jval_set_path(n, "output.restoreClipboard", gs_jval_bool(true));
    gs_jval_set_path(n, "stt.engine", gs_jval_str("builtin"));
    gs_jval_set_path(n, "cleanup.enabled", gs_jval_bool(Z.cleanup_enabled));
    gs_jval_set_path(n, "cleanup.engine", gs_jval_str("builtin"));
    const eh_model *m = cleanup_model();
    if (m) gs_jval_set_path(n, "cleanup.builtin.model", gs_jval_str(m->id));
    gs_jval_set_path(n, "cleanup.style", gs_jval_str(eh_styles[Z.style].id));
    gs_jval_set_path(n, "audio.deviceId", gs_jval_str(Z.mic > 0 ? Z.mics[Z.mic] : ""));
    return n;
}

/* ---------------- the last step ---------------- */

static bool row_done(const download_row *row) {
    char dir[1200];
    return row->skipped || eh_model_installed(row->model, dir, sizeof dir);
}

static bool all_done(void) {
    for (int i = 0; i < Z.row_count; i++)
        if (!row_done(&Z.rows[i])) return false;
    return true;
}

static void enter_downloads(void) {
    const eh_model *needed[2];
    int n = 0;
    const eh_model *stt = eh_catalog_find(EH_MODEL_STT, gs_jval_string(gs_jval_path(Z.base, "stt.builtin.model"), NULL));
    if (!stt) stt = eh_catalog_find(EH_MODEL_STT, NULL);
    if (stt) needed[n++] = stt;
    if (Z.cleanup_enabled && cleanup_model()) needed[n++] = cleanup_model();
    bool same = Z.started && n == Z.row_count;
    for (int i = 0; same && i < n; i++) same = Z.rows[i].model == needed[i];
    if (same) return;
    // A model no longer wanted stops downloading.
    for (int i = 0; i < Z.row_count; i++) {
        bool kept = false;
        for (int j = 0; j < n; j++) kept = kept || needed[j] == Z.rows[i].model;
        if (!kept) eh_model_job_cancel(Z.rows[i].model);
    }
    Z.row_count = n;
    char dir[1200], error[200];
    for (int i = 0; i < n; i++) {
        Z.rows[i] = (download_row){ needed[i], false };
        if (!eh_model_installed(needed[i], dir, sizeof dir)) eh_model_job_start(needed[i], error, sizeof error);
    }
    Z.started = true;
}

static void finish_task(void *arg) {
    eh_hotkey_results results;
    char error[256];
    if (!eh_commit_settings(arg, &Z.baseline, &results, error, sizeof error)) {
        SDL_snprintf(Z.finish_status, sizeof Z.finish_status, "Could not save: %s", error);
        Z.finish_failed = true;
        Z.saving = false;
        return;
    }
    gs_jval_free(Z.base);
    Z.base = gs_jval_clone(eh_settings());
    eh_form_baseline_take(&Z.baseline, Z.base);
    Z.saving = false;
    Z.finish_status[0] = 0;
    if (results.r[EH_HK_RECORD].ok) {
        Z.outcome = 1;
        return;
    }
    // The one thing the wizard cannot finish without: back to its step, with the reason.
    SDL_snprintf(Z.hotkey_status, sizeof Z.hotkey_status, "%s — choose a different combination, then finish setup again.", results.r[EH_HK_RECORD].error);
    Z.outcome = 2;
}

/* ---------------- the steps ---------------- */

static struct { float x, w, y; } pg;

static void text(const char *s, float px, SDL_FColor c) {
    pg.y += eh_w_text(Z.ui, (gs_rect){ pg.x, pg.y, pg.w, 400 }, s, px, c);
}

static gs_rect take(float h) {
    gs_rect r = { pg.x, pg.y, pg.w, h };
    pg.y += h;
    return r;
}

static void heading(const char *s) {
    text(s, 18, gs_rgba(EH_TEXT, 1));
    pg.y += 10;
}

static void para(const char *s) {
    text(s, 13, gs_rgba(EH_TEXT_DIM, 1));
    pg.y += 14;
}

static void field_label(const char *s) {
    eh_w_line(Z.ui, take(20), s, 13, gs_rgba(EH_TEXT, 1), -1);
    pg.y += 6;
}

static void key_cap(float *x, float y, const char *name) {
    float w = gs_ui_text_width(eh_ui_gs(Z.ui), name, 12) + 16;
    eh_w_round(Z.ui, (gs_rect){ *x, y, w, 26 }, 6, gs_rgba(0xffffff, 0.1f));
    eh_w_outline(Z.ui, (gs_rect){ *x, y, w, 26 }, 6, gs_rgba(0xffffff, 0.2f));
    eh_w_line(Z.ui, (gs_rect){ *x, y, w, 26 }, name, 12, gs_rgba(EH_TEXT, 1), 0);
    *x += w + 5;
}

static void welcome(void) {
    para("Earheart is private, hotkey-driven voice dictation. There is no account, no cloud requirement and nothing to learn — using it looks like this:");
    // Three small pictures: the keys, the bar, the words.
    float col = (pg.w - 40) / 3, top = pg.y, s = gs_ui_scale(eh_ui_gs(Z.ui));
    SDL_Renderer *r = SDL_GetRenderer(eh_ui_window(Z.ui));
    static const char *const captions[3] = { "1. Press the hotkey anywhere", "2. Speak, press it again", "3. Text lands where you type" };
    for (int i = 0; i < 3; i++) {
        gs_rect box = { pg.x + (float)i * (col + 20), top, col, 76 };
        eh_w_round(Z.ui, box, 12, EH_WASH_FAINT);
        eh_w_outline(Z.ui, box, 12, EH_EDGE);
        if (i == 0) {
#ifdef __APPLE__
            static const char *const keys[3] = { "Cmd", "Shift", "Space" };
#else
            static const char *const keys[3] = { "Ctrl", "Shift", "Space" };
#endif
            float total = 10;
            for (int k = 0; k < 3; k++) total += gs_ui_text_width(eh_ui_gs(Z.ui), keys[k], 12) + 21;
            float x = box.x + (box.w - total + 15) / 2;
            for (int k = 0; k < 3; k++) key_cap(&x, box.y + 25, keys[k]);
        } else if (i == 1) {
            gs_rect bar = { box.x + 14, box.y + 24, box.w - 28, 28 };
            eh_w_round(Z.ui, bar, 14, gs_rgba(EH_INK, 1));
            eh_w_outline(Z.ui, bar, 14, EH_EDGE);
            gs_draw_circle(r, (bar.x + 14) * s, (bar.y + 14) * s, 3.5f * s, gs_rgba(EH_ACCENT, 1));
            static const float heights[13] = { 4, 9, 14, 7, 11, 16, 6, 12, 9, 15, 5, 10, 7 };
            for (int k = 0; k < 13; k++) eh_w_round(Z.ui, (gs_rect){ bar.x + 26 + (float)k * 4, bar.y + 14 - heights[k] / 2, 2.5f, heights[k] }, 1.2f, gs_rgba(EH_ACCENT, 1));
            eh_w_line(Z.ui, (gs_rect){ bar.x + bar.w - 62, bar.y, 30, bar.h }, "0:03", 11, gs_rgba(EH_TEXT, 1), 1);
            gs_draw_circle(r, (bar.x + bar.w - 14) * s, (bar.y + 14) * s, 9 * s, gs_rgba(EH_TEXT, 1));
            float cx = (bar.x + bar.w - 14) * s, cy = (bar.y + 14) * s, u = s * 0.5f;
            gs_draw_line(r, cx - 4.6f * u * 1.6f, cy + 0.6f * u * 1.6f, cx - 1.3f * u * 1.6f, cy + 3.8f * u * 1.6f, 1.8f * s, gs_rgba(EH_INK, 1));
            gs_draw_line(r, cx - 1.3f * u * 1.6f, cy + 3.8f * u * 1.6f, cx + 4.6f * u * 1.6f, cy - 3.4f * u * 1.6f, 1.8f * s, gs_rgba(EH_INK, 1));
        } else {
            gs_rect line = { box.x + 16, box.y + 26, box.w - 32, 24 };
            eh_w_line(Z.ui, line, "Your words appear.", 13, gs_rgba(EH_TEXT, 1), 0);
        }
        eh_w_line(Z.ui, (gs_rect){ box.x, top + 84, col, 20 }, captions[i], 12, gs_rgba(EH_TEXT_DIM, 1), 0);
        if (i < 2) {
            float ax = (box.x + col + 5) * s, ay = (top + 38) * s;
            gs_draw_line(r, ax, ay, ax + 10 * s, ay, 1.5f * s, gs_rgba(EH_TEXT_DIM, 1));
            gs_draw_line(r, ax + 6 * s, ay - 4 * s, ax + 10 * s, ay, 1.5f * s, gs_rgba(EH_TEXT_DIM, 1));
            gs_draw_line(r, ax + 6 * s, ay + 4 * s, ax + 10 * s, ay, 1.5f * s, gs_rgba(EH_TEXT_DIM, 1));
        }
    }
    pg.y = top + 120;
    para("This wizard pre-configures everything; you can review and change all of it on the settings screen afterwards.");
}

static void hotkey_step(void) {
    heading("Pick your dictation hotkey");
    para("One key combination toggles everything: press to start dictating, press again to stop and transcribe.");
    field_label("Global hotkey");
    gs_rect r = take(38);
    float bw = eh_w_button_width(Z.ui, "Reset");
    char got[64];
    if (eh_w_hotkey(Z.ui, (gs_rect){ r.x, r.y, r.w - bw - 10, r.h }, "hotkey", Z.hotkey, "Click, then press a key combination", got, sizeof got)) {
        SDL_strlcpy(Z.hotkey, got, sizeof Z.hotkey);
        Z.hotkey_status[0] = 0;
    }
    if (eh_w_button(Z.ui, (gs_rect){ r.x + r.w - bw, r.y + 2, bw, 34 }, "hotkey-clear", "Reset", EH_BTN_PLAIN)) {
        gs_jval *d = gs_jval_parse(eh_defaults_json, sizeof eh_defaults_json - 1);
        SDL_strlcpy(Z.hotkey, gs_jval_string(gs_jval_get(d, "hotkey"), ""), sizeof Z.hotkey);
        gs_jval_free(d);
    }
    pg.y += 8;
#ifdef __linux__
    text("The default works fine for most people — click the box to choose your own. On GNOME/KDE Wayland, apps may not be able to grab global keys — instead, bind a system keyboard shortcut to earheart --toggle.", 12, gs_rgba(EH_TEXT_DIM, 1));
#else
    text("The default works fine for most people — click the box to choose your own.", 12, gs_rgba(EH_TEXT_DIM, 1));
#endif
    if (Z.hotkey_status[0]) {
        pg.y += 8;
        text(Z.hotkey_status, 12, gs_rgba(EH_ERR, 1));
    }
}

static void mic_step(void) {
    heading("Choose a microphone");
    para("The system default is usually right. Pick a specific device if you have more than one.");
    field_label("Microphone");
    eh_w_select(Z.ui, take(36), "mic-device", (const char *const *)Z.mics, Z.mic_count, &Z.mic);
    if (Z.no_mics) {
        pg.y += 8;
        text("No microphone found (or access was denied). You can pick one later in Settings.", 12, gs_rgba(EH_TEXT_DIM, 1));
    }
}

static void bullet(const char *s) {
    float sc = gs_ui_scale(eh_ui_gs(Z.ui));
    gs_draw_circle(SDL_GetRenderer(eh_ui_window(Z.ui)), (pg.x + 8) * sc, (pg.y + 9) * sc, 2.5f * sc, gs_rgba(EH_OK, 1));
    pg.y += eh_w_text(Z.ui, (gs_rect){ pg.x + 22, pg.y, pg.w - 22, 100 }, s, 13, gs_rgba(EH_TEXT, 1)) + 6;
}

static void stt_step(void) {
    heading("Speech becomes text on this computer");
    para("Earheart runs NVIDIA's Parakeet model right inside the app — nothing to install, no separate program.");
    bullet("Your voice never leaves this computer");
    bullet("Faster than real time, even without a graphics card");
    bullet("Understands 25 languages automatically");
    pg.y += 8;
    para("The model (~660 MB) downloads in the next step, with a progress bar.");
    para("Prefer a remote service (OpenAI, Groq, your own server)? You can switch to one anytime in Settings.");
}

static void cleanup_step(void) {
    heading("Polish your transcripts automatically?");
    para("After transcription, a small language model can fix punctuation and capitalization and remove filler words and false starts — so what lands is clean, not a raw stream of speech.");
    gs_rect row = take(26);
    eh_w_line(Z.ui, (gs_rect){ row.x, row.y, row.w - 60, row.h }, "Clean up transcripts automatically (recommended)", 13, gs_rgba(EH_TEXT, 1), -1);
    eh_w_switch(Z.ui, (gs_rect){ row.x + row.w - 40, row.y + 2, 40, 22 }, "cleanup-enabled", &Z.cleanup_enabled);
    pg.y += 16;
    eh_ui_inert(Z.ui, !Z.cleanup_enabled, !Z.cleanup_enabled);
    text("Runs a language model inside the app (Qwen3 4B Instruct by default) — like an editor that tidies every dictation for you, fully offline.", 13, eh_ui_ink(Z.ui, EH_TEXT_DIM, 1));
    pg.y += 14;
    field_label("Quality");
    const eh_model *list[32];
    const char *labels[32];
    int n = cleanup_models(list, 32);
    for (int i = 0; i < n; i++) labels[i] = list[i]->label ? list[i]->label : list[i]->id;
    eh_w_select(Z.ui, take(36), "cleanup-builtin-model", labels, n, &Z.cleanup_model);
    const eh_model *m = cleanup_model();
    if (m && m->note) {
        char note[320], dir[1200];
        SDL_snprintf(note, sizeof note, "%s%s", m->note, eh_model_installed(m, dir, sizeof dir) ? " · already downloaded" : "");
        pg.y += 7;
        text(note, 12, eh_ui_ink(Z.ui, EH_TEXT_DIM, 1));
    }
    pg.y += 14;
    field_label("Cleanup style");
    eh_w_stops(Z.ui, take(28), "cleanup-style", &Z.style, (int)SDL_arraysize(eh_styles));
    gs_rect ends = take(18);
    eh_w_line(Z.ui, ends, "Closer to your words", 12, eh_ui_ink(Z.ui, EH_TEXT_DIM, 1), -1);
    eh_w_line(Z.ui, ends, "More polished", 12, eh_ui_ink(Z.ui, EH_TEXT_DIM, 1), 1);
    pg.y += 8;
    char line[200];
    SDL_snprintf(line, sizeof line, "%s — %s", eh_styles[Z.style].label, eh_styles[Z.style].hint);
    text(line, 13, eh_ui_ink(Z.ui, EH_TEXT, 1));
    pg.y += 14;
    text("Prefer an OpenAI-compatible service (Ollama, LM Studio, OpenAI)? You can point cleanup at one anytime in Settings.", 12, eh_ui_ink(Z.ui, EH_TEXT_DIM, 1));
    eh_ui_inert(Z.ui, false, false);
}

static void summary_row(const char *name, const char *value) {
    gs_rect r = take(26);
    eh_w_line(Z.ui, (gs_rect){ r.x, r.y, 130, r.h }, name, 12, gs_rgba(EH_TEXT_DIM, 1), -1);
    eh_w_line(Z.ui, (gs_rect){ r.x + 130, r.y, r.w - 130, r.h }, value, 13, gs_rgba(EH_TEXT, 1), -1);
}

static void output_step(void) {
    heading("Where should the text go?");
    static const char *const modes[3] = { "Paste into the active app", "Paste and keep the transcript on the clipboard", "Copy to clipboard only — I'll paste myself" };
    static const char *const ids[3] = { "output-paste", "output-paste-copy", "output-clipboard" };
    for (int i = 0; i < 3; i++)
        if (eh_w_radio(Z.ui, take(30), ids[i], modes[i], Z.output_mode == i)) Z.output_mode = i;
    pg.y += 18;
    field_label("Your setup");
    float top = pg.y;
    eh_w_round(Z.ui, (gs_rect){ pg.x, top, pg.w, 5 * 26 + 20 }, 12, EH_WASH_FAINT);
    eh_w_outline(Z.ui, (gs_rect){ pg.x, top, pg.w, 5 * 26 + 20 }, 12, EH_EDGE);
    float x = pg.x, w = pg.w;
    pg.x += 16, pg.w -= 32, pg.y += 10;
    char cleanup[200];
    const eh_model *m = cleanup_model();
    if (!Z.cleanup_enabled) SDL_strlcpy(cleanup, "Off", sizeof cleanup);
    else SDL_snprintf(cleanup, sizeof cleanup, "Built-in %s (on this computer) · %s", m && m->label ? m->label : "", eh_styles[Z.style].label);
    summary_row("Hotkey", Z.hotkey);
    summary_row("Microphone", Z.mics[Z.mic]);
    summary_row("Speech-to-text", "Built-in Parakeet (on this computer)");
    summary_row("Cleanup", cleanup);
    summary_row("Output", Z.output_mode == 2 ? "Clipboard only" : Z.output_mode == 1 ? "Paste and keep on clipboard" : "Paste into active app");
    pg.x = x, pg.w = w, pg.y += 10;
}

static void finish_step(void) {
    enter_downloads();
    char dir[1200];
    int waiting = 0;
    for (int i = 0; i < Z.row_count; i++) waiting += !eh_model_installed(Z.rows[i].model, dir, sizeof dir);
    heading(waiting ? "Setting up the models that run on your computer" : "You're all set");
    if (waiting) para("These run entirely on your machine, so your dictation stays private and works offline. This is a one-time download.");
    for (int i = 0; i < Z.row_count; i++) {
        download_row *row = &Z.rows[i];
        const eh_model *m = row->model;
        eh_model_job job = eh_model_job_get(m);
        bool installed = eh_model_installed(m, dir, sizeof dir), failed = !installed && job.done && !job.ok && !job.cancelled;
        char line[240];
        unsigned tone = EH_TEXT_DIM;
        float fraction = installed ? 1 : job.total ? (float)((double)job.received / (double)job.total) : 0;
        if (installed) SDL_strlcpy(line, "Ready", sizeof line), tone = EH_OK;
        else if (row->skipped) SDL_strlcpy(line, "Skipped — download later in Settings", sizeof line);
        else if (job.active && job.total) SDL_snprintf(line, sizeof line, "%.0f%% · %.0f MB / %.0f MB", floor((double)fraction * 100 + 0.5), (double)job.received / 1e6, (double)job.total / 1e6);
        else if (job.active) SDL_strlcpy(line, "Downloading…", sizeof line);
        else if (job.cancelled) SDL_strlcpy(line, "Cancelled", sizeof line);
        else if (failed) SDL_strlcpy(line, job.error[0] ? job.error : "Download failed", sizeof line), tone = EH_ERR;
        else SDL_strlcpy(line, "Downloading…", sizeof line);
        gs_rect head = take(22);
        eh_w_line(Z.ui, (gs_rect){ head.x, head.y, head.w * 0.55f, head.h }, m->label ? m->label : m->id, 13, gs_rgba(EH_TEXT, 1), -1);
        eh_w_line(Z.ui, (gs_rect){ head.x + head.w * 0.4f, head.y, head.w * 0.6f, head.h }, line, 12, gs_rgba(tone, 1), 1);
        pg.y += 6;
        gs_rect bar = take(4);
        eh_w_round(Z.ui, bar, 2, gs_rgba(0xffffff, 0.12f));
        if (fraction > 0) eh_w_round(Z.ui, (gs_rect){ bar.x, bar.y, bar.w * SDL_min(1, fraction), bar.h }, 2, gs_rgba(EH_TEXT, 1));
        if (m->note) pg.y += 8, text(m->note, 12, gs_rgba(EH_TEXT_DIM, 1));
        if (failed && !row->skipped) {
            pg.y += 8;
            char id[32];
            SDL_snprintf(id, sizeof id, "retry-%d", i);
            float w = eh_w_button_width(Z.ui, "Retry");
            if (eh_w_button(Z.ui, (gs_rect){ pg.x, pg.y, w, 34 }, id, "Retry", EH_BTN_PLAIN)) {
                char error[200];
                eh_model_job_start(m, error, sizeof error);
            }
            pg.y += 34;
        }
        pg.y += 18;
        if (job.active) eh_ui_again(Z.ui, 100);
    }
    if (!all_done()) {
        float w = eh_w_button_width(Z.ui, "Download later (from Settings)");
        if (eh_w_button(Z.ui, (gs_rect){ pg.x, pg.y, w, 34 }, "download-later", "Download later (from Settings)", EH_BTN_PLAIN)) {
            for (int i = 0; i < Z.row_count; i++) {
                if (row_done(&Z.rows[i])) continue;
                eh_model_job_cancel(Z.rows[i].model);
                Z.rows[i].skipped = true;
            }
        }
        pg.y += 44;
    }
    if (Z.finish_status[0]) text(Z.finish_status, 12, gs_rgba(Z.finish_failed ? EH_ERR : EH_TEXT_DIM, 1));
}

/* ---------------- the window ---------------- */

void eh_wizard_window_open(void) {
    if (!Z.ui) {
        Z.ui = eh_ui_new("Welcome to Earheart", 620, 680, 560, 560);
        if (!Z.ui) return;
        eh_lock();
        gs_jval_free(Z.base);
        Z.base = gs_jval_clone(eh_settings());
        load();
        eh_unlock();
        Z.step = 0;
        Z.started = false;
        Z.row_count = 0;
        Z.hotkey_status[0] = Z.finish_status[0] = 0;
        Z.outcome = 0;
        Z.saving = false;
    }
    SDL_ShowWindow(eh_ui_window(Z.ui));
    SDL_RaiseWindow(eh_ui_window(Z.ui));
    eh_app_activate();
}

bool eh_wizard_window_is_open(void) { return Z.ui != NULL; }

void eh_wizard_window_close(void) {
    if (!Z.ui) return;
    eh_lock();
    if (eh_ui_capturing(Z.ui)) eh_hotkeys_suspend(false);
    // Closed before the end, downloads it began stop: nothing was saved that would want them.
    if (Z.outcome != 1)
        for (int i = 0; i < Z.row_count; i++) eh_model_job_cancel(Z.rows[i].model);
    Z.started = false;
    eh_unlock();
    eh_ui_free(Z.ui);
    Z.ui = NULL;
}

bool eh_wizard_window_event(const SDL_Event *e) { return Z.ui && eh_ui_event(Z.ui, e); }

struct eh_ui *eh_wizard_window_ui(void) { return Z.ui; }
int eh_wizard_window_step(void) { return Z.step; }

int eh_wizard_window_frame(void) {
    if (!Z.ui) return -1;
    if (eh_ui_close_asked(Z.ui)) {
        eh_wizard_window_close();
        return -1;
    }
    if (Z.outcome == 1) {
        // Saved: the settings window takes over, saying where its values came from.
        eh_wizard_window_close();
        Z.outcome = 0;
        eh_settings_window_open_from_wizard();
        return -1;
    }
    if (Z.outcome == 3) {
        eh_wizard_window_close();
        Z.outcome = 0;
        eh_settings_window_open(NULL);
        return -1;
    }
    if (Z.outcome == 2) Z.step = STEP_HOTKEY, Z.outcome = 0;
    eh_lock();
    bool was_capturing = eh_ui_capturing(Z.ui);
    gs_rect area = eh_ui_begin(Z.ui);
    gs_ui *gs = eh_ui_gs(Z.ui);

    gs_rect header = gs_cut_top(&area, 72);
    eh_w_icon(Z.ui, (gs_rect){ 24, 18, 34, 34 });
    eh_w_line(Z.ui, (gs_rect){ 71, 16, header.w - 90, 22 }, "Welcome to Earheart", 16, gs_rgba(EH_TEXT, 1), -1);
    eh_w_line(Z.ui, (gs_rect){ 71, 38, header.w - 90, 18 }, "A couple of quick questions and you're dictating.", 12, gs_rgba(EH_TEXT_DIM, 1), -1);
    gs_ui_fill(gs, (gs_rect){ 0, header.h - 1, header.w, 1 }, EH_SEAM);
    gs_rect footer = gs_cut_bottom(&area, 64);
    gs_ui_fill(gs, (gs_rect){ 0, footer.y, footer.w, 1 }, EH_SEAM);

    pg.x = area.x + 32, pg.w = area.w - 64, pg.y = area.y + 26;
    switch (Z.step) {
    case STEP_WELCOME: welcome(); break;
    case STEP_HOTKEY: hotkey_step(); break;
    case STEP_MIC: mic_step(); break;
    case STEP_STT: stt_step(); break;
    case STEP_CLEANUP: cleanup_step(); break;
    case STEP_OUTPUT: output_step(); break;
    default: finish_step(); break;
    }

    // The footer: skip, where we are, back and next.
    float y = footer.y + 15, x = 24, s = gs_ui_scale(gs);
    float skip_w = eh_w_button_width(Z.ui, "Skip setup");
    bool skip = eh_w_button(Z.ui, (gs_rect){ x, y, skip_w, 34 }, "skip", "Skip setup", EH_BTN_NAV);
    float dots_x = footer.w / 2 - (float)STEPS * 7;
    for (int i = 0; i < STEPS; i++) gs_draw_circle(SDL_GetRenderer(eh_ui_window(Z.ui)), (dots_x + (float)i * 14 + 4) * s, (footer.y + 32) * s, 3 * s, gs_rgba(EH_TEXT, i == Z.step ? 1 : 0.25f));
    bool last = Z.step == STEPS - 1, ready = !last || all_done();
    const char *next_label = Z.step == 0 ? "Get started" : !last ? "Next" : ready ? "Finish setup" : "Downloading…";
    float next_w = eh_w_button_width(Z.ui, next_label) + 16, back_w = eh_w_button_width(Z.ui, "Back");
    eh_ui_inert(Z.ui, !ready || Z.saving, false);
    bool next = eh_w_button(Z.ui, (gs_rect){ footer.w - 24 - next_w, y, next_w, 34 }, "next", next_label, EH_BTN_PRIMARY);
    eh_ui_inert(Z.ui, false, false);
    bool back = Z.step > 0 && eh_w_button(Z.ui, (gs_rect){ footer.w - 24 - next_w - 10 - back_w, y, back_w, 34 }, "back", "Back", EH_BTN_PLAIN);
    if (back) Z.step--;
    if (next && !last) Z.step++;
    if (next && last) {
        Z.saving = true;
        Z.finish_failed = false;
        SDL_strlcpy(Z.finish_status, "Saving…", sizeof Z.finish_status);
        eh_spawn(finish_task, collect());
    }
    if (skip) {
        // The settings as they are become the file, so the wizard is not shown again.
        char error[256];
        eh_settings_save(gs_jval_clone(eh_settings()), error, sizeof error);
        Z.outcome = 3;
    }
    if (Z.saving) eh_ui_again(Z.ui, 50);
    if (Z.outcome) eh_ui_again(Z.ui, 0);
    if (eh_ui_capturing(Z.ui) != was_capturing) eh_hotkeys_suspend(eh_ui_capturing(Z.ui));
    int again = eh_ui_end(Z.ui);
    eh_unlock();
    return again;
}
