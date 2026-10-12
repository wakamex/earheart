// Turn-taking for always-on listening (src/listen.c): where speech starts and stops, given the
// voice detector's answer for each 10 ms frame.
#include "listen.h"
#include "test.h"

// Feeds `count` frames of one probability; returns the frame (from 1) at which `want` came, or 0,
// and fails on any other change.
static int feed(eh_turn *t, float probability, int count, int want) {
    int at = 0;
    for (int i = 1; i <= count; i++) {
        int step = eh_turn_step(t, probability);
        if (step && (step != want || at)) return -1;
        if (step) at = i;
    }
    return at;
}

void test_listen(void) {
    eh_turn t = { 0 };
    // Quiet is quiet, and a blip shorter than the start length starts nothing.
    CHECK(feed(&t, 0.1f, 300, 0) == 0 && !t.speaking);
    CHECK(feed(&t, 0.9f, EH_TURN_START_FRAMES - 1, 0) == 0 && feed(&t, 0.1f, 1, 0) == 0 && !t.speaking);
    // Speech starts on the frame that completes the run, and one half counts as speech.
    CHECK(feed(&t, 0.5f, 20, 1) == EH_TURN_START_FRAMES && t.speaking);
    // A pause shorter than the stop length is part of the speech; speech after it begins the count again.
    CHECK(feed(&t, 0.0f, EH_TURN_STOP_FRAMES - 1, -1) == 0 && t.speaking);
    CHECK(feed(&t, 0.99f, 1, 0) == 0);
    CHECK(feed(&t, 0.49f, EH_TURN_STOP_FRAMES + 30, -1) == EH_TURN_STOP_FRAMES && !t.speaking);
    // And it starts again afterwards, with a full run.
    CHECK(feed(&t, 0.9f, EH_TURN_START_FRAMES - 1, 1) == 0);
    CHECK(feed(&t, 0.9f, 1, 1) == 1 && t.speaking);
}
