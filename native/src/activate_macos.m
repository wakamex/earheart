#import <AppKit/AppKit.h>
#include "activate.h"

void eh_app_activate(void) {
    [NSApp activateIgnoringOtherApps:YES];
}
