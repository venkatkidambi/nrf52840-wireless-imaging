#ifndef IMAGE_CONTROL_H
#define IMAGE_CONTROL_H
#ifndef IMAGE_CONTROL_PROFILE
#define IMAGE_CONTROL_PROFILE 0
#endif
#if IMAGE_CONTROL_PROFILE < 0 || IMAGE_CONTROL_PROFILE > 2
#error "Unknown image control profile"
#endif
/* Request and reply guards protect different peer paths. See control-audit.md.
 * Baseline remains the default. Reduced values require physical validation. */
#ifndef IMAGE_REQUEST_GUARD_US
#define IMAGE_REQUEST_GUARD_US (IMAGE_CONTROL_PROFILE == 2 ? 1500u : 3000u)
#endif
#ifndef IMAGE_REPLY_GUARD_US
#define IMAGE_REPLY_GUARD_US (IMAGE_CONTROL_PROFILE == 0 ? 3000u : \
                            IMAGE_CONTROL_PROFILE == 1 ? 1500u : 1000u)
#endif
#if IMAGE_REQUEST_GUARD_US < 1000 || IMAGE_REPLY_GUARD_US < 1000 || \
    IMAGE_REQUEST_GUARD_US > 3000 || IMAGE_REPLY_GUARD_US > 3000
#error "Control guards must be within the characterized candidate range 1000..3000 us"
#endif
#endif
