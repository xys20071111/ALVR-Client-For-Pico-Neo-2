package top.playtbsxys.picostreamer;

import android.content.SharedPreferences;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import android.view.WindowManager;

import com.picovr.vractivity.VRActivity;
import com.picovr.vractivity.Eye;
import com.picovr.vractivity.HmdState;
import com.picovr.vractivity.RenderInterface;
import com.picovr.cvclient.CVControllerManager;
import com.picovr.cvclient.CVController;
import com.picovr.cvclient.CVControllerListener;
import com.picovr.cvclient.ButtonNum;
import com.picovr.picovrlib.cvcontrollerclient.ControllerClient;
import com.psmart.vrlib.PvrClient;

public class PicoALVRActivity extends VRActivity implements RenderInterface {

    private static final String TAG = "PicoALVR";

    // PvrClient for direct 6DoF tracking data access (HmdState.getPos() may return zeros)
    private PvrClient mPvrClient;

    // Frame counter for periodic diagnostic logging
    private int mFrameCount = 0;
    // Previous button state for change detection
    private boolean[] mPrevLeftBtns = new boolean[6];
    private boolean[] mPrevRightBtns = new boolean[6];

    static {
        System.loadLibrary("pico_alvr");
    }

    private CVControllerManager mControllerManager;
    private CVController mLeftController;
    private CVController mRightController;

    // Pico Home key (key code 1001) state, tracked via dispatchKeyEvent override
    private static boolean sHomeKeyDown = false;

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        int keyCode = event.getKeyCode();
        // Pico Neo 2 Home button uses custom key code 1001.
        // VRActivity.dispatchKeyEvent remaps it to 96; we intercept before that.
        if (keyCode == 1001) {
            int action = event.getAction();
            if (action == KeyEvent.ACTION_DOWN) {
                sHomeKeyDown = true;
                Log.i(TAG, "Home key DOWN (intercepted)");
            } else if (action == KeyEvent.ACTION_UP) {
                sHomeKeyDown = false;
                Log.i(TAG, "Home key UP (intercepted)");
            }
            return true; // Consume, prevent system launcher
        }
        return super.dispatchKeyEvent(event);
    }

    private void setupControllers() {
        mLeftController = mControllerManager.getLeftController();
        mRightController = mControllerManager.getRightController();
        // Disable Pico system Home key intercept so it reaches SteamVR via ALVR
        if (mLeftController != null) mLeftController.CVControllerSetIsEnbleHomeKey(false);
        if (mRightController != null) mRightController.CVControllerSetIsEnbleHomeKey(false);
        Log.i(TAG, "Controllers setup, HomeKey intercept disabled");
    }

    private final CVControllerListener mControllerListener = new CVControllerListener() {
        @Override
        public void onBindSuccess() {
            Log.i(TAG, "Controller service bound");
        }

        @Override
        public void onBindFail() {
            Log.e(TAG, "Controller service bind failed");
        }

        @Override
        public void onThreadStart() {
            Log.i(TAG, "Controller thread started");
            setupControllers();
        }

        @Override
        public void onConnectStateChanged(int state, int type) {
            Log.i(TAG, "Controller connect state: " + state + " type: " + type);
            setupControllers();
        }

        @Override
        public void onMainControllerChanged(int controller) {
            Log.i(TAG, "Main controller changed: " + controller);
        }

        @Override
        public void onChannelChanged(int channel, int type) {
            Log.i(TAG, "Controller channel changed: " + channel + " type: " + type);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        mControllerManager = new CVControllerManager(getApplicationContext());
        mControllerManager.setListener(mControllerListener);
        mPvrClient = new PvrClient(this);  // PvrClient requires Activity context, not Application

        // Load user-configured stream settings (FOV half-angle in degrees, standing height in meters)
        SharedPreferences prefs = getSharedPreferences("stream_settings", MODE_PRIVATE);
        float fovH = prefs.getFloat("fov_h", 55.0f);
        float fovV = prefs.getFloat("fov_v", 55.0f);
        float height = prefs.getFloat("standing_height", 1.5f);
        Log.i(TAG, "Stream config from prefs: fovH=" + fovH + " fovV=" + fovV + " height=" + height);
        setStreamConfigNative(fovH, fovV, height);

        initializeNative();
        mPvrClient.startVRMode();
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Stop Pico's HomeKeyLongPressReceiver so it doesn't trigger system launcher
        VRActivity.Pvr_StopHomeKeyLongPressReceiver(this);
        mControllerManager.bindService();
        if (mPvrClient != null) {
            mPvrClient.onResume();
            // Enable 6DoF tracking mode (0=3DoF, 1=6DoF)
            int result = mPvrClient.setTrackingMode(1); // 1 = 6DoF
            Log.i(TAG, "setTrackingMode(6DoF) result: " + result);
        }
        resumeNative();
    }

    @Override
    protected void onPause() {
        mControllerManager.unbindService();
        if (mPvrClient != null) mPvrClient.onPause();
        pauseNative();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        if (mPvrClient != null) mPvrClient.onDestroy();
        destroyNative();
        super.onDestroy();
    }

    // ===== RenderInterface callbacks =====

    @Override
    public void initGL(int width, int height) {
        Log.i(TAG, "initGL: " + width + "x" + height);
        initGLNative(width, height);
    }

    @Override
    public void deInitGL() {
        Log.i(TAG, "deInitGL");
        deInitGLNative();
    }

    @Override
    public void onFrameBegin(HmdState hmdState) {
        // Get HMD orientation and position
        float[] hmdOrientation = new float[4];
        float[] hmdPosition = new float[3];
        hmdState.getOrientation(hmdOrientation, 0);
        hmdState.getPos(hmdPosition, 0);

        // 6DoF position: setTrackingMode(1) in onResume should make HmdState.getPos()
        // return real position data. PvrClient.getTrackingDataExt() was causing NPE
        // when PVR service disconnected (e.g. after SteamVR restart).

        // Update controller data with HMD pose (required by PicoVR SDK)
        float[] hmdData = new float[7];
        hmdData[0] = hmdOrientation[0];
        hmdData[1] = hmdOrientation[1];
        hmdData[2] = hmdOrientation[2];
        hmdData[3] = hmdOrientation[3];
        hmdData[4] = hmdPosition[0];
        hmdData[5] = hmdPosition[1];
        hmdData[6] = hmdPosition[2];

        if (mControllerManager != null) {
            mControllerManager.updateControllerData(hmdData);
        }

        // Gather controller data
        float[] leftOri = null, leftPos = null;
        float[] rightOri = null, rightPos = null;
        int leftTrigger = 0, rightTrigger = 0;
        int[] leftTouchpad = null, rightTouchpad = null;
        boolean[] leftButtons = null, rightButtons = null;
        int leftBattery = 0, rightBattery = 0;
        boolean leftConnected = false, rightConnected = false;

        if (mLeftController != null && mLeftController.getConnectState() == 1) {
            leftConnected = true;
            leftOri = mLeftController.getOrientation();
            leftPos = mLeftController.getPosition();
            leftTrigger = mLeftController.getTriggerNum();
            leftTouchpad = mLeftController.getTouchPad();
            leftButtons = new boolean[8];
            leftButtons[0] = sHomeKeyDown || mLeftController.getButtonState(ButtonNum.home);
            leftButtons[1] = mLeftController.getButtonState(ButtonNum.app);
            leftButtons[2] = mLeftController.getButtonState(ButtonNum.click);
            leftButtons[3] = mLeftController.getButtonState(ButtonNum.buttonAX);
            leftButtons[4] = mLeftController.getButtonState(ButtonNum.buttonBY);
            leftButtons[5] = mLeftController.getButtonState(ButtonNum.buttonLG);
            leftBattery = mLeftController.getBatteryLevel();

            // Pico Neo 2 workaround: standard getButtonState() doesn't reliably report
            // grip/A/X/B/Y. Read directly from CV2 key event API for reliable data.
            int lSerial = mLeftController.getSerialNum();
            int[] lCv2Keys = ControllerClient.getCV2ControllerKeyEvent(lSerial);
            if (lCv2Keys != null && lCv2Keys.length >= 4) {
                leftButtons[3] = lCv2Keys[0] == 1; // buttonAX
                leftButtons[4] = lCv2Keys[1] == 1; // buttonBY
                leftButtons[5] = lCv2Keys[2] == 1; // grip (buttonLG)
            }
            // Fallback: if CV2 API reports no grip, try standard API
            if (!leftButtons[5]) {
                leftButtons[5] = mLeftController.getButtonState(ButtonNum.buttonLG);
            }

            // Temporary diagnostic: log all CV2 key data on any left button change
            boolean lChanged = false;
            for (int i = 0; i < 6; i++) { if (leftButtons[i] != mPrevLeftBtns[i]) { lChanged = true; mPrevLeftBtns[i] = leftButtons[i]; } }
            // Also log periodically to capture grip press even if standard API misses it
            if (lChanged || mFrameCount % 600 == 1) {
                int[] lStdKeys = ControllerClient.getControllerKeyEvent(lSerial);
                Log.i(TAG, "L diag: cv2=" + java.util.Arrays.toString(lCv2Keys)
                    + " std=" + java.util.Arrays.toString(lStdKeys)
                    + " grip_std=" + mLeftController.getButtonState(ButtonNum.buttonLG)
                    + " trig=" + leftTrigger);
            }
        }

        if (mRightController != null && mRightController.getConnectState() == 1) {
            rightConnected = true;
            rightOri = mRightController.getOrientation();
            rightPos = mRightController.getPosition();
            rightTrigger = mRightController.getTriggerNum();
            rightTouchpad = mRightController.getTouchPad();
            rightButtons = new boolean[8];
            rightButtons[0] = sHomeKeyDown || mRightController.getButtonState(ButtonNum.home);
            rightButtons[1] = mRightController.getButtonState(ButtonNum.app);
            rightButtons[2] = mRightController.getButtonState(ButtonNum.click);
            rightButtons[3] = mRightController.getButtonState(ButtonNum.buttonAX);
            rightButtons[4] = mRightController.getButtonState(ButtonNum.buttonBY);
            rightButtons[5] = mRightController.getButtonState(ButtonNum.buttonRG);
            rightBattery = mRightController.getBatteryLevel();

            // Pico Neo 2 workaround: standard getButtonState() doesn't reliably report
            // grip/A/X/B/Y. Read directly from CV2 key event API for reliable data.
            int rSerial = mRightController.getSerialNum();
            int[] rCv2Keys = ControllerClient.getCV2ControllerKeyEvent(rSerial);
            if (rCv2Keys != null && rCv2Keys.length >= 4) {
                rightButtons[3] = rCv2Keys[0] == 1; // buttonAX
                rightButtons[4] = rCv2Keys[1] == 1; // buttonBY
                rightButtons[5] = rCv2Keys[2] == 1; // grip (buttonRG)
            }

            // Diagnostic: log on button state change only
            boolean rChanged = false;
            for (int i = 0; i < 6; i++) { if (rightButtons[i] != mPrevRightBtns[i]) { rChanged = true; mPrevRightBtns[i] = rightButtons[i]; } }
            if (rChanged) {
                Log.i(TAG, "R btns: click=" + rightButtons[2]
                    + " ax=" + rightButtons[3] + " by=" + rightButtons[4]
                    + " grip=" + rightButtons[5] + " trig=" + rightTrigger
                    + " cv2=" + java.util.Arrays.toString(rCv2Keys));
            }
            mFrameCount++;
        }

        onFrameBeginNative(
            hmdOrientation, hmdPosition,
            leftConnected, leftOri, leftPos, leftTrigger, leftTouchpad, leftButtons, leftBattery,
            rightConnected, rightOri, rightPos, rightTrigger, rightTouchpad, rightButtons, rightBattery
        );
    }

    @Override
    public void onDrawEye(Eye eye) {
        int eyeIndex = eye.getType();  // 0 = left, 1 = right
        onDrawEyeNative(eyeIndex);
    }

    @Override
    public void onFrameEnd() {
        onFrameEndNative();
    }

    @Override
    public void onTouchEvent() {
        // Handle touch events if needed
    }

    @Override
    public void surfaceChangedCallBack(int width, int height) {
        Log.i(TAG, "surfaceChanged: " + width + "x" + height);
    }

    @Override
    public void onRenderPause() {
        Log.i(TAG, "onRenderPause");
        pauseNative();
    }

    @Override
    public void onRenderResume() {
        Log.i(TAG, "onRenderResume");
        resetStreamStateNative();
        resumeNative();
    }

    @Override
    public void onRendererShutdown() {
        Log.i(TAG, "onRendererShutdown");
        deInitGLNative();
    }

    @Override
    public void renderEventCallBack(int event) {
        // Handle VR system events if needed
    }

    // ===== Native methods =====

    private native void setStreamConfigNative(float fovH, float fovV, float height);
    private native void initializeNative();
    private native void resumeNative();
    private native void pauseNative();
    private native void destroyNative();
    private native void resetStreamStateNative();
    private native void initGLNative(int width, int height);
    private native void deInitGLNative();

    private native void onFrameBeginNative(
        float[] hmdOrientation, float[] hmdPosition,
        boolean leftConnected, float[] leftOri, float[] leftPos,
        int leftTrigger, int[] leftTouchpad, boolean[] leftButtons, int leftBattery,
        boolean rightConnected, float[] rightOri, float[] rightPos,
        int rightTrigger, int[] rightTouchpad, boolean[] rightButtons, int rightBattery
    );

    private native void onDrawEyeNative(int eye);
    private native void onFrameEndNative();
}
