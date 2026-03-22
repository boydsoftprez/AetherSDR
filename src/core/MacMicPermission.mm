#import <AVFoundation/AVFoundation.h>

void requestMicrophonePermission()
{
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio
                             completionHandler:^(BOOL granted) {
        // Nothing to do — the system prompt is what matters.
        // If granted, QAudioSource will work when PTT is pressed.
        // If denied, the user can fix it in System Settings.
    }];
}
