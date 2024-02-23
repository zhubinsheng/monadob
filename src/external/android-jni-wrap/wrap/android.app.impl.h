// Copyright 2020-2021, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
// Author: Rylie Pavlik <rylie.pavlik@collabora.com>
// Inline implementations: do not include on its own!

#pragma once

#include "android.content.h"
#include <string>

namespace wrap {
namespace android::app {

inline jni::Object Activity::getWindow() {
    assert(!isNull());
    return object().call<jni::Object>(Meta::data().getWindow);
}

inline void
Activity::setVrModeEnabled(bool enabled,
                           content::ComponentName const &requestedComponent) {
    assert(!isNull());
    return object().call<void>(Meta::data().setVrModeEnabled, enabled,
                               requestedComponent.object());
}

} // namespace android::app
} // namespace wrap
