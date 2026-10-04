// FastPlay's options, by name, for the settings screens: the same options as the
// desktop's Options window keeps, saved in the same settings file. Setting one
// applies it as the desktop's OK button does, and saves.

#import <Foundation/Foundation.h>

#import "FPEngine.h"

NS_ASSUME_NONNULL_BEGIN

@interface FPEngine (Settings)

// A numeric or yes/no option (yes is 1). The names are in FPSettings.mm.
- (double)numberForSetting:(NSString*)name NS_SWIFT_NAME(number(forSetting:));
- (void)setNumber:(double)value forSetting:(NSString*)name NS_SWIFT_NAME(setNumber(_:forSetting:));

// An option that is a file ("convolutionIR", "midiSoundFont"): the file's name, or
// "" if none is chosen.
- (NSString*)fileNameForSetting:(NSString*)name NS_SWIFT_NAME(fileName(forSetting:));
// Chooses the file at `path` (copied into FastPlay's own data, so it stays), or
// with nil, none. False if the file could not be copied or loaded.
- (BOOL)setFile:(nullable NSString*)path forSetting:(NSString*)name NS_SWIFT_NAME(setFile(_:forSetting:));

@end

NS_ASSUME_NONNULL_END
