// PK1: first-run disc picker (macOS). Runs in a child process (`<exe> --ps2x-pick-disc [message]`)
// spawned by ps2x::disc::resolveLaunch, so this plain NSApplication never coexists with the game's
// SDL3/raylib application object. Exit 0 = a disc was validated, prepared and stored in general.json.
#include "ps2_disc_setup.h"

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace ps2x::disc
{
    namespace
    {
        NSString *ns(const std::string &s) { return [NSString stringWithUTF8String:s.c_str()] ?: @""; }

        // NSAlertFirstButtonReturn = primary.
        NSModalResponse alert(NSAlertStyle style, NSString *title, NSString *text, NSString *primary, NSString *secondary)
        {
            NSAlert *a = [[NSAlert alloc] init];
            a.alertStyle = style;
            a.messageText = title;
            a.informativeText = text;
            [a addButtonWithTitle:primary];
            if (secondary)
                [a addButtonWithTitle:secondary];
            [NSApp activateIgnoringOtherApps:YES];
            return [a runModal];
        }
    }

    int runPickerProcess(int argc, char *argv[])
    {
        @autoreleasepool
        {
            [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
            [NSApp activateIgnoringOtherApps:YES];

            NSString *intro = argc >= 3 && argv[2] && argv[2][0]
                                  ? ns(argv[2])
                                  : @"Road Trip Adventure runs from your own PAL disc (SLES-51356). Choose an ISO image, or the "
                                    @".cue/.bin of your disc. The game files are copied once into Application Support; "
                                    @"no disc data comes with this app.";
            if (alert(NSAlertStyleInformational, @"Choose your Road Trip Adventure disc", intro, @"Choose Disc…", @"Quit") !=
                NSAlertFirstButtonReturn)
                return 2;

            for (;;)
            {
                NSOpenPanel *panel = [NSOpenPanel openPanel];
                panel.title = @"Choose your Road Trip Adventure disc image";
                panel.message = @"ISO, CUE or BIN image of the PAL disc SLES-51356";
                panel.prompt = @"Use Disc";
                panel.canChooseFiles = YES;
                panel.canChooseDirectories = NO;
                panel.allowsMultipleSelection = NO;
                NSMutableArray<UTType *> *types = [NSMutableArray array];
                for (NSString *ext in @[ @"iso", @"cue", @"bin", @"img" ])
                    if (UTType *t = [UTType typeWithFilenameExtension:ext])
                        [types addObject:t];
                if (types.count)
                    panel.allowedContentTypes = types;
                [NSApp activateIgnoringOtherApps:YES];
                if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0)
                    return 2;

                const std::filesystem::path picked = panel.URLs.firstObject.fileSystemRepresentation;
                const std::filesystem::path image = resolveImagePath(picked);
                Result r = validate(image);
                if (r.ok())
                {
                    std::filesystem::path elf, iso;
                    r = prepare(image, elf, iso);
                    if (r.ok() && storeDisc(image))
                        return 0;
                    if (r.ok())
                        r = Result{Status::PrepareFailed, "Could not save your disc choice in " + (configDir() / "general.json").string() +
                                   ". If that file exists but is damaged, fix or delete it and try again."};
                }
                if (alert(NSAlertStyleCritical, @"This disc can't be used", ns(r.message), @"Choose Another…", @"Quit") !=
                    NSAlertFirstButtonReturn)
                    return 2;
            }
        }
    }
}
