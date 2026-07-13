package top.playtbsxys.picostreamer;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.SeekBar;
import android.widget.TextView;

/**
 * Settings screen shown before VR streaming starts.
 * User can adjust FOV (horizontal half-angle) and standing height.
 * Values are persisted to SharedPreferences "stream_settings".
 */
public class SettingsActivity extends Activity {

    private static final String PREFS_NAME = "stream_settings";

    // FOV: SeekBar progress 0..50 maps to half-angle 30..80 degrees
    private static final float FOV_MIN = 30.0f;
    private static final float FOV_MAX = 80.0f;
    private static final float FOV_DEFAULT = 55.0f;

    // Height: SeekBar progress 0..100 maps to 1.00..2.00 meters
    private static final float HEIGHT_MIN = 1.00f;
    private static final float HEIGHT_MAX = 2.00f;
    private static final float HEIGHT_DEFAULT = 1.50f;

    private SeekBar mSbFov;
    private SeekBar mSbHeight;
    private TextView mTvFovValue;
    private TextView mTvHeightValue;
    private SharedPreferences mPrefs;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);

        // Fix window size for Pico VR shell panel display
//        getWindow().setLayout(900, 600);

        mPrefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        mSbFov = findViewById(R.id.sb_fov);
        mSbHeight = findViewById(R.id.sb_height);
        mTvFovValue = findViewById(R.id.tv_fov_value);
        mTvHeightValue = findViewById(R.id.tv_height_value);
        Button btnSave = findViewById(R.id.btn_save);
        Button btnReset = findViewById(R.id.btn_reset);

        // Load saved values
        float savedFov = mPrefs.getFloat("fov_h", FOV_DEFAULT);
        float savedHeight = mPrefs.getFloat("standing_height", HEIGHT_DEFAULT);

        mSbFov.setMax((int)(FOV_MAX - FOV_MIN));
        mSbFov.setProgress((int)(savedFov - FOV_MIN));

        mSbHeight.setMax((int)((HEIGHT_MAX - HEIGHT_MIN) * 100));
        mSbHeight.setProgress((int)((savedHeight - HEIGHT_MIN) * 100));

        updateFovLabel();
        updateHeightLabel();

        mSbFov.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) { updateFovLabel(); }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        mSbHeight.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) { updateHeightLabel(); }
            @Override public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override public void onStopTrackingTouch(SeekBar seekBar) {}
        });

        btnSave.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                float fovH = getFovValue();
                float height = getHeightValue();
                mPrefs.edit()
                    .putFloat("fov_h", fovH)
                    .putFloat("fov_v", fovH)  // square eye buffer: same h/v
                    .putFloat("standing_height", height)
                    .apply();
                // Launch VR activity
                Intent intent = new Intent(SettingsActivity.this, PicoALVRActivity.class);
                startActivity(intent);
                finish();
            }
        });

        btnReset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                mSbFov.setProgress((int)(FOV_DEFAULT - FOV_MIN));
                mSbHeight.setProgress((int)((HEIGHT_DEFAULT - HEIGHT_MIN) * 100));
                updateFovLabel();
                updateHeightLabel();
            }
        });
    }

    private float getFovValue() {
        return FOV_MIN + mSbFov.getProgress();
    }

    private float getHeightValue() {
        return HEIGHT_MIN + mSbHeight.getProgress() / 100.0f;
    }

    private void updateFovLabel() {
        float fov = getFovValue();
        // Show full FOV (2 * half-angle) for user-friendliness
        mTvFovValue.setText(String.format("水平全角 %.0f°  (半角 %.0f°)", fov * 2, fov));
    }

    private void updateHeightLabel() {
        float height = getHeightValue();
        int cm = Math.round(height * 100);
        mTvHeightValue.setText(String.format("%.2f m  (%d cm)", height, cm));
    }
}
