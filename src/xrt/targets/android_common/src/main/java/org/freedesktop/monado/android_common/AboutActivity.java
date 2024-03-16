// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Simple main activity for Android.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 */

package org.freedesktop.monado.android_common;

import android.app.Activity;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.text.method.LinkMovementMethod;
import android.util.Log;
import android.view.View;
import android.widget.ImageView;
import android.widget.TextView;
import androidx.appcompat.app.AppCompatActivity;
import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.app.ActivityCompat;
import androidx.fragment.app.Fragment;
import androidx.fragment.app.FragmentTransaction;
import dagger.hilt.android.AndroidEntryPoint;
import javax.inject.Inject;
import org.freedesktop.monado.auxiliary.NameAndLogoProvider;
import org.freedesktop.monado.auxiliary.UiProvider;

@AndroidEntryPoint
public class AboutActivity extends AppCompatActivity {

    @Inject NoticeFragmentProvider noticeFragmentProvider;

    @Inject UiProvider uiProvider;

    @Inject NameAndLogoProvider nameAndLogoProvider;

    private boolean isInProcessBuild() {
        try {
            getClassLoader().loadClass("org/freedesktop/monado/ipc/Client");
            return false;
        } catch (ClassNotFoundException e) {
            // ok, we're in-process.
        }
        return true;
    }

    //动态申请权限
    private static final int REQUEST_EXTERNAL_STORAGE = 1;
    private static String[] PERMISSIONS_STORAGE={
            "android.permission.READ_EXTERNAL_STORAGE",
            "android.permission.WRITE_EXTERNAL_STORAGE"
    };
    //权限申请函数，必须在onCreate里面调用
    public static void verifyStoragePermissions(Activity activity){
        try {
            int permisssion= ActivityCompat.checkSelfPermission(activity,"android.permission.WRITE_EXTERNAL_STORAGE" );
            if (permisssion!= PackageManager.PERMISSION_GRANTED){
                ActivityCompat.requestPermissions(activity,PERMISSIONS_STORAGE,REQUEST_EXTERNAL_STORAGE);//弹出权限申请对话框
            }
            Log.i("zbs", "permisssion" + permisssion);

            Log.i("zbs", "getExternalFilesDir" + activity.getExternalFilesDir(null).getAbsolutePath());
        }catch (Exception e){
            e.printStackTrace();
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_about);
        verifyStoragePermissions(this);

        // Default to dark mode universally?
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES);

        // Make our Monado link clickable
        ((TextView) findViewById(R.id.textPowered))
                .setMovementMethod(LinkMovementMethod.getInstance());

        // Branding from the branding provider
        ((TextView) findViewById(R.id.textName))
                .setText(nameAndLogoProvider.getLocalizedRuntimeName());
        ((ImageView) findViewById(R.id.imageView))
                .setImageDrawable(nameAndLogoProvider.getLogoDrawable());

        boolean isInProcess = isInProcessBuild();
        if (!isInProcess) {
            ShutdownProcess.Companion.setupRuntimeShutdownButton(this);
        }

        // Start doing fragments
        FragmentTransaction fragmentTransaction = getSupportFragmentManager().beginTransaction();

        @VrModeStatus.Status
        int status =
                VrModeStatus.detectStatus(
                        this, getApplicationContext().getApplicationInfo().packageName);

        VrModeStatus statusFrag = VrModeStatus.newInstance(status);
        fragmentTransaction.add(R.id.statusFrame, statusFrag, null);

        if (!isInProcess) {
            findViewById(R.id.drawOverOtherAppsFrame).setVisibility(View.VISIBLE);
            DisplayOverOtherAppsStatusFragment drawOverFragment =
                    new DisplayOverOtherAppsStatusFragment();
            fragmentTransaction.replace(R.id.drawOverOtherAppsFrame, drawOverFragment, null);
        }

        if (noticeFragmentProvider != null) {
            Fragment noticeFragment = noticeFragmentProvider.makeNoticeFragment();
            fragmentTransaction.add(R.id.aboutFrame, noticeFragment, null);
        }

        fragmentTransaction.commit();
    }
}
