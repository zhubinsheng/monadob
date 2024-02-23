// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Provides a "Notice" fragment using AboutLibraries.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 */

package org.freedesktop.monado.openxr_runtime

import androidx.fragment.app.Fragment
import com.mikepenz.aboutlibraries.LibsBuilder
import javax.inject.Inject
import org.freedesktop.monado.android_common.NoticeFragmentProvider

class AboutLibrariesNoticeFragmentProvider @Inject constructor() : NoticeFragmentProvider {
    override fun makeNoticeFragment(): Fragment =
        LibsBuilder()
            .withFields(R.string::class.java.fields)
            .withAboutIconShown(false) // We do this ourselves bigger
            .withAboutVersionShown(true) // Let the fragment show our version
            .withLicenseShown(true) // Not sure why you'd do this without license info
            .supportFragment()
}
