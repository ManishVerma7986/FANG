package com.fang.animatedgles;

import android.app.Activity;
import android.content.res.Configuration;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.PowerManager;
import android.os.SystemClock;
import android.util.Log;
import android.util.AtomicFile;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.WindowManager;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;

import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

public final class AnimatedGlesActivity extends Activity {
    private static final String TAG = "FangAnimatedGles";
    private static final String WORKLOAD_ID = "fang-animated-gles";
    private static final int REQUESTED_WIDTH = 1080;
    private static final int REQUESTED_HEIGHT = 2400;
    private static final int REQUESTED_GLES_MAJOR = 3;
    private static final float REQUESTED_REFRESH_HZ = 60.0f;
    private static final long NANOS_PER_SECOND = 1_000_000_000L;
    private static final float DEGREES_PER_SECOND = 45.0f;

    private final Object eventLock = new Object();
    private final List<String> events = new ArrayList<>();
    private final AtomicBoolean terminal = new AtomicBoolean();

    private String runId;
    private String buildId;
    private int warmupSeconds;
    private int durationSeconds;
    private boolean diagnosticMode;
    private File runDirectory;
    private AnimatedSurface surface;
    private AnimatedRenderer renderer;
    private int nextSequence;
    private long runStartedNs;
    private String terminalStatus;
    private String terminalReason;
    private String thermalStartEvidence;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON
                | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION);
        try {
            runId = UUID.fromString(requireExtra("runId")).toString();
            buildId = requireExtra("buildId");
            if (!buildId.matches("[A-Fa-f0-9]{12,64}")) {
                throw new IllegalArgumentException("buildId must be a hexadecimal source identifier.");
            }
            warmupSeconds = getIntent().getIntExtra("warmupSeconds", -1);
            durationSeconds = getIntent().getIntExtra("durationSeconds", -1);
            if (warmupSeconds < 1 || warmupSeconds > 30
                    || durationSeconds < 2 || durationSeconds > 120) {
                throw new IllegalArgumentException("Warm-up or measurement duration is outside the supported range.");
            }
            diagnosticMode = getIntent().getBooleanExtra("diagnosticMode", false);
            runDirectory = new File(new File(getFilesDir(), "runs"), runId);
            if (!runDirectory.mkdirs() && !runDirectory.isDirectory()) {
                throw new IOException("Could not create a unique run directory.");
            }
        } catch (Exception exception) {
            Log.e(TAG, "Invalid launch configuration; no run was started.", exception);
            finish();
            return;
        }

        runStartedNs = SystemClock.elapsedRealtimeNanos();
        thermalStartEvidence = thermalStateEvidenceJson();
        recordEvent("activity_created", "\"build_id\":\"" + escape(buildId) + "\"");
        Display display = getWindowManager().getDefaultDisplay();
        recordEvent("display_observed", displayEventFields(display));

        surface = new AnimatedSurface(this);
        surface.setEGLContextClientVersion(REQUESTED_GLES_MAJOR);
        surface.setPreserveEGLContextOnPause(false);
        renderer = new AnimatedRenderer();
        surface.setRenderer(renderer);
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        setContentView(surface);
    }

    @Override
    protected void onStart() {
        super.onStart();
        if (runId != null && !terminal.get()) {
            recordEvent("activity_started", "");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (surface != null) {
            surface.onResume();
        }
        if (runId != null && !terminal.get()) {
            recordEvent("activity_resumed", "");
        }
    }

    @Override
    protected void onPause() {
        if (runId != null && !terminal.get()) {
            recordEvent("activity_paused", "");
        }
        if (surface != null) {
            surface.onPause();
        }
        if (runId != null && !terminal.get()) {
            finishRun("interrupted", "lifecycle_interruption");
        }
        super.onPause();
    }

    @Override
    protected void onStop() {
        if (runId != null && !terminal.get()) {
            recordEvent("activity_stopped", "");
            finishRun("interrupted", "activity_stopped_before_completion");
        }
        super.onStop();
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (runId != null && !terminal.get()) {
            recordEvent("configuration_changed",
                    "\"orientation\":" + newConfig.orientation);
            finishRun("interrupted", "configuration_changed");
        }
    }

    @Override
    protected void onDestroy() {
        if (runId != null && !terminal.get()) {
            recordEvent("activity_destroyed", "");
            finishRun("interrupted", "activity_destroyed_before_completion");
        }
        super.onDestroy();
    }

    private String requireExtra(String name) {
        String value = getIntent().getStringExtra(name);
        if (value == null || value.isEmpty()) {
            throw new IllegalArgumentException("Required launch extra is missing: " + name);
        }
        return value;
    }

    private String displayEventFields(Display display) {
        Display.Mode mode = display.getMode();
        int orientation = getResources().getConfiguration().orientation;
        String orientationName = orientation == Configuration.ORIENTATION_PORTRAIT
                ? "portrait" : orientation == Configuration.ORIENTATION_LANDSCAPE
                ? "landscape" : "unknown";
        return "\"display_mode_width_px\":" + mode.getPhysicalWidth()
                + ",\"display_mode_height_px\":" + mode.getPhysicalHeight()
                + ",\"display_refresh_hz\":" + format(mode.getRefreshRate())
                + ",\"orientation\":\"" + orientationName + "\"";
    }

    private void recordEvent(String type, String fields) {
        synchronized (eventLock) {
            long timestampNs = SystemClock.elapsedRealtimeNanos();
            String suffix = fields == null || fields.isEmpty() ? "" : "," + fields;
            events.add("{\"run_id\":\"" + escape(runId) + "\",\"sequence\":"
                    + (++nextSequence) + ",\"clock_domain\":\"guest_monotonic\",\"type\":\"" + escape(type)
                    + "\",\"guest_monotonic_ns\":" + timestampNs + suffix + "}");
        }
    }

    private void finishRun(String status, String reason) {
        if (!terminal.compareAndSet(false, true)) {
            return;
        }
        terminalStatus = status;
        terminalReason = reason;
        String fields = "\"status\":\"" + escape(status) + "\"";
        if (reason != null) {
            fields += ",\"reason\":\"" + escape(reason) + "\"";
        }
        recordEvent(status.equals("completed") ? "run_completed" : "run_interrupted", fields);
        try {
            writeGuestResult();
            Log.i(TAG, "Completed result production for run_id=" + runId
                    + " validity=" + getRunValidity());
        } catch (Exception exception) {
            Log.e(TAG, "RESULT_PRODUCTION_FAILED run_id=" + runId, exception);
        }
        if (!isFinishing()) {
            runOnUiThread(() -> finish());
        }
    }

    private String getRunValidity() {
        if ("interrupted".equals(terminalStatus)) {
            return "invalid";
        }
        if ("failed".equals(terminalStatus)) {
            return "failed";
        }
        if (diagnosticMode) {
            return "diagnostic";
        }
        return renderer != null && renderer.isProfileConforming()
                ? "valid" : "invalid";
    }

    private void writeGuestResult() throws IOException, NoSuchAlgorithmException {
        List<String> eventSnapshot;
        synchronized (eventLock) {
            eventSnapshot = new ArrayList<>(events);
        }
        ByteArrayOutputStream eventBytes = new ByteArrayOutputStream();
        for (String event : eventSnapshot) {
            eventBytes.write(event.getBytes(StandardCharsets.UTF_8));
            eventBytes.write('\n');
        }
        byte[] rawEvents = eventBytes.toByteArray();
        writeAtomic(new File(runDirectory, "events.jsonl"), rawEvents);

        long endedNs = SystemClock.elapsedRealtimeNanos();
        long sceneReadyNs = renderer == null ? 0 : renderer.sceneReadyNs;
        long measurementStartNs = renderer == null ? 0 : renderer.measurementStartNs;
        long measurementEndNs = renderer == null ? 0 : renderer.measurementEndNs;
        int frameSamples = renderer == null ? 0 : renderer.frameSamples;
        boolean profileConforming = renderer != null && renderer.isProfileConforming();
        String invalidationReason = terminalReason;
        if ("completed".equals(terminalStatus) && !profileConforming) {
            invalidationReason = diagnosticMode ? "diagnostic_profile_mismatch" : "requested_profile_mismatch";
        } else if ("completed".equals(terminalStatus) && diagnosticMode) {
            invalidationReason = "diagnostic_mode";
        }
        if ("completed".equals(terminalStatus) && frameSamples == 0) {
            invalidationReason = "no_measurement_samples";
        }
        String validity = getRunValidity();
        if ("completed".equals(terminalStatus) && frameSamples == 0) {
            validity = "invalid";
        }
        if (renderer != null && renderer.failureReason != null) {
            validity = "failed";
            invalidationReason = renderer.failureReason;
        }

        String json = "{"
                + "\"schema_version\":1,"
                + "\"kind\":\"fang-animated-gles-run\","
                + "\"run_id\":\"" + escape(runId) + "\","
                + "\"workload\":{\"identifier\":\"" + WORKLOAD_ID + "\",\"version\":1,\"build_id\":\""
                + escape(buildId) + "\"},"
                + "\"requested_profile\":{\"width_px\":" + REQUESTED_WIDTH
                + ",\"height_px\":" + REQUESTED_HEIGHT
                + ",\"orientation\":\"portrait\",\"gles_major\":" + REQUESTED_GLES_MAJOR
                + ",\"refresh_hz\":" + format(REQUESTED_REFRESH_HZ) + "},"
                + "\"observed_profile\":" + (renderer == null ? "{}" : renderer.observedProfileJson()) + ","
                + "\"run\":{\"status\":\"" + escape(terminalStatus)
                + "\",\"validity\":\"" + validity
                + "\",\"diagnostic_mode\":" + diagnosticMode
                + ",\"profile_conforming\":" + profileConforming
                + ",\"started_guest_monotonic_ns\":" + runStartedNs
                + ",\"ended_guest_monotonic_ns\":" + endedNs
                + ",\"scene_ready_guest_monotonic_ns\":" + sceneReadyNs
                + ",\"warmup_duration_ns\":" + (long) warmupSeconds * NANOS_PER_SECOND
                + ",\"measurement_duration_requested_ns\":" + (long) durationSeconds * NANOS_PER_SECOND
                + ",\"measurement_start_guest_monotonic_ns\":" + measurementStartNs
                + ",\"measurement_end_guest_monotonic_ns\":" + measurementEndNs
                + ",\"invalidation_reason\":" + nullableString(invalidationReason) + "},"
                + "\"thermal_state\":{\"start\":" + thermalStartEvidence
                + ",\"end\":" + thermalStateEvidenceJson() + "},"
                + "\"clock_domains\":{\"guest\":\"android.os.SystemClock.elapsedRealtimeNanos; monotonic, includes device sleep\","
                + "\"host\":\"not_combined; host QPC is collected separately\"},"
                + "\"metrics\":" + metricsJson(frameSamples) + ","
                + "\"errors\":" + (renderer == null ? "[]" : renderer.errorsJson()) + ","
                + "\"artifacts\":{\"events_jsonl\":{\"path\":\"events.jsonl\",\"sha256\":\""
                + sha256(rawEvents) + "\",\"bytes\":" + rawEvents.length
                + ",\"event_count\":" + eventSnapshot.size() + "}}"
                + "}";
        writeAtomic(new File(runDirectory, "guest-result.json"), json.getBytes(StandardCharsets.UTF_8));
    }

    private String metricsJson(int frameSamples) {
        if (frameSamples == 0) {
            return "{"
                    + "\"guest_update_duration_ns\":{\"status\":\"unavailable\",\"source\":\"renderer CPU update interval\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"reason\":\"No measurement frame samples were captured.\"},"
                    + "\"cpu_submission_duration_ns\":{\"status\":\"unavailable\",\"source\":\"GLES submission call interval\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"reason\":\"No measurement frame samples were captured.\"},"
                    + "\"draw_count\":{\"status\":\"unavailable\",\"source\":\"GLES draw call counter\",\"unit\":\"calls_per_callback\",\"clock_domain\":\"guest_monotonic\",\"reason\":\"No measurement frame samples were captured.\"},"
                    + "\"callback_interval_ns\":{\"status\":\"unavailable\",\"source\":\"GLSurfaceView.Renderer callback entry timestamps\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"reason\":\"No measurement frame samples were captured.\"},"
                    + unavailableMetric("gpu_execution_duration_ns", "ns", null, "A validated GPU timer-query source is not implemented.") + ","
                    + unavailableMetric("actual_presentation_interval_ns", "ns", null, "No app-correlated presentation source is validated.") + ","
                    + unavailableMetric("missed_presentation_deadlines", "deadlines", null, "No app-correlated presentation deadline source is validated.")
                    + "}";
        }
        return "{"
                + "\"guest_update_duration_ns\":{\"status\":\"collected\",\"source\":\"renderer CPU update interval\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"sample_count\":" + frameSamples + "},"
                + "\"cpu_submission_duration_ns\":{\"status\":\"collected\",\"source\":\"GLES submission API-call interval; excludes GPU completion\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"sample_count\":" + frameSamples + "},"
                + "\"draw_count\":{\"status\":\"collected\",\"source\":\"GLES draw call counter\",\"unit\":\"calls_per_callback\",\"clock_domain\":\"guest_monotonic\",\"sample_count\":" + frameSamples + "},"
                + callbackMetricJson(frameSamples) + ","
                + unavailableMetric("gpu_execution_duration_ns", "ns", null, "A validated GPU timer-query source is not implemented.") + ","
                + unavailableMetric("actual_presentation_interval_ns", "ns", null, "No app-correlated presentation source is validated.") + ","
                + unavailableMetric("missed_presentation_deadlines", "deadlines", null, "No app-correlated presentation deadline source is validated.")
                + "}";
    }

    private String callbackMetricJson(int frameSamples) {
        if (frameSamples < 2) {
            return "\"callback_interval_ns\":{\"status\":\"unavailable\",\"source\":\"GLSurfaceView.Renderer callback entry timestamps; not presentation\",\"value\":null,\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"reason\":\"At least two measurement callbacks are required to form an interval.\"}";
        }
        return "\"callback_interval_ns\":{\"status\":\"collected\",\"source\":\"GLSurfaceView.Renderer callback entry timestamps; not presentation\",\"unit\":\"ns\",\"clock_domain\":\"guest_monotonic\",\"sample_count\":" + (frameSamples - 1) + "}";
    }

    private String unavailableMetric(String name, String unit, String clockDomain, String reason) {
        return "\"" + name + "\":{\"status\":\"not_yet_validated\",\"source\":null,\"value\":null,\"unit\":\""
                + escape(unit) + "\",\"clock_domain\":" + nullableString(clockDomain)
                + ",\"reason\":\"" + escape(reason) + "\"}";
    }

    private String metricsEvent(long updateNs, long submissionNs, long callbackIntervalNs) {
        String callback = callbackIntervalNs < 0 ? "null" : Long.toString(callbackIntervalNs);
        return "\"metrics\":{\"guest_update_duration_ns\":" + updateNs
                + ",\"cpu_submission_duration_ns\":" + submissionNs
                + ",\"draw_count\":4,\"callback_interval_ns\":" + callback + "}";
    }

    private String thermalStateEvidenceJson() {
        if (android.os.Build.VERSION.SDK_INT < 29) {
            return "{\"status\":\"unavailable\",\"source\":\"PowerManager.getCurrentThermalStatus\","
                    + "\"value\":null,\"reason\":\"Android API level is below 29.\"}";
        }
        PowerManager powerManager = (PowerManager) getSystemService(POWER_SERVICE);
        if (powerManager == null) {
            return "{\"status\":\"unavailable\",\"source\":\"PowerManager.getCurrentThermalStatus\","
                    + "\"value\":null,\"reason\":\"PowerManager service is unavailable.\"}";
        }
        return "{\"status\":\"collected\",\"source\":\"PowerManager.getCurrentThermalStatus\","
                + "\"value\":" + powerManager.getCurrentThermalStatus() + "}";
    }

    private void writeAtomic(File target, byte[] bytes) throws IOException {
        AtomicFile atomic = new AtomicFile(target);
        FileOutputStream output = atomic.startWrite();
        try {
            output.write(bytes);
            output.getFD().sync();
            atomic.finishWrite(output);
        } catch (IOException exception) {
            atomic.failWrite(output);
            throw exception;
        }
    }

    private static String sha256(byte[] bytes) throws NoSuchAlgorithmException {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(bytes);
        StringBuilder result = new StringBuilder(digest.length * 2);
        for (byte value : digest) {
            result.append(String.format(Locale.US, "%02x", value & 0xff));
        }
        return result.toString();
    }

    private static String evidenceNumber(String source, String value) {
        try {
            if (Double.parseDouble(value) <= 0.0) {
                return "{\"status\":\"unavailable\",\"source\":\"" + escape(source)
                        + "\",\"value\":null,\"reason\":\"The runtime did not provide a positive value.\"}";
            }
        } catch (NumberFormatException exception) {
            return "{\"status\":\"unavailable\",\"source\":\"" + escape(source)
                    + "\",\"value\":null,\"reason\":\"The runtime did not provide a numeric value.\"}";
        }
        return "{\"status\":\"collected\",\"source\":\"" + escape(source) + "\",\"value\":" + value + "}";
    }

    private static String evidenceString(String source, String value) {
        if (value == null || value.isEmpty()) {
            return "{\"status\":\"unavailable\",\"source\":\"" + escape(source)
                    + "\",\"value\":null,\"reason\":\"The runtime did not provide a value.\"}";
        }
        return "{\"status\":\"collected\",\"source\":\"" + escape(source)
                + "\",\"value\":\"" + escape(value) + "\"}";
    }

    private static String nullableString(String value) {
        return value == null ? "null" : "\"" + escape(value) + "\"";
    }

    private static String escape(String value) {
        if (value == null) {
            return "";
        }
        StringBuilder escaped = new StringBuilder(value.length() + 16);
        for (int index = 0; index < value.length(); index++) {
            char character = value.charAt(index);
            if (character == '"' || character == '\\') {
                escaped.append('\\').append(character);
            } else if (character == '\n') {
                escaped.append("\\n");
            } else if (character == '\r') {
                escaped.append("\\r");
            } else if (character == '\t') {
                escaped.append("\\t");
            } else if (character < 0x20) {
                escaped.append(String.format(Locale.US, "\\u%04x", (int) character));
            } else {
                escaped.append(character);
            }
        }
        return escaped.toString();
    }

    private static String format(float value) {
        return String.format(Locale.US, "%.6f", value);
    }

    private final class AnimatedSurface extends GLSurfaceView {
        AnimatedSurface(Activity activity) {
            super(activity);
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {
            super.surfaceCreated(holder);
            recordEvent("surface_created", "");
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            recordEvent("surface_destroyed", "");
            if (!terminal.get()) {
                finishRun("interrupted", "surface_destroyed");
            }
            super.surfaceDestroyed(holder);
        }
    }

    private final class AnimatedRenderer implements GLSurfaceView.Renderer {
        private static final int SEGMENTS = 96;
        private static final float RING_STROKE = 0.012f;
        private static final float SQUARE_HALF_SIZE = 0.08f;
        private final float[] radii = {0.25f, 0.50f, 0.75f};
        private final int[] buffers = new int[4];
        private final float[][] ringVertices = new float[3][];
        private final float[] squareVertices = {
                -SQUARE_HALF_SIZE, -SQUARE_HALF_SIZE,
                SQUARE_HALF_SIZE, -SQUARE_HALF_SIZE,
                SQUARE_HALF_SIZE, SQUARE_HALF_SIZE,
                -SQUARE_HALF_SIZE, -SQUARE_HALF_SIZE,
                SQUARE_HALF_SIZE, SQUARE_HALF_SIZE,
                -SQUARE_HALF_SIZE, SQUARE_HALF_SIZE
        };
        private static final String VERTEX_SHADER =
                "#version 300 es\n"
                        + "in vec2 aPosition;\n"
                        + "uniform vec2 uViewportScale;\n"
                        + "uniform float uAngleRadians;\n"
                        + "uniform int uRotate;\n"
                        + "void main(){vec2 p=aPosition;if(uRotate==1){float c=cos(uAngleRadians);float s=sin(uAngleRadians);p=vec2(c*p.x-s*p.y,s*p.x+c*p.y);}gl_Position=vec4(p*uViewportScale,0.0,1.0);}\n";
        private static final String FRAGMENT_SHADER =
                "#version 300 es\nprecision mediump float;\n"
                        + "uniform vec4 uColor;\n"
                        + "out vec4 outColor;\n"
                        + "void main(){outColor=uColor;}\n";

        private final List<String> errors = new ArrayList<>();
        private int program;
        private int positionLocation;
        private int viewportScaleLocation;
        private int angleLocation;
        private int rotateLocation;
        private int colorLocation;
        private int width;
        private int height;
        private float viewportScaleX;
        private float viewportScaleY;
        private String glVersion;
        private String glVendor;
        private String glRenderer;
        private float refreshHz;
        private long sceneReadyNs;
        private long measurementStartNs;
        private long measurementEndNs;
        private long lastMeasuredCallbackNs = -1;
        private int frameSamples;
        private boolean sceneReadyRecorded;
        private boolean measurementStarted;
        private boolean profileConforming;
        private String failureReason;

        @Override
        public void onSurfaceCreated(GL10 ignored, EGLConfig config) {
            if (sceneReadyRecorded && !terminal.get()) {
                failureReason = "egl_context_recreated";
                recordEvent("egl_context_recreated", "");
                finishRun("interrupted", failureReason);
                return;
            }
            GLES30.glClearColor(16.0f / 255.0f, 20.0f / 255.0f, 32.0f / 255.0f, 1.0f);
            glVersion = GLES30.glGetString(GLES30.GL_VERSION);
            glVendor = GLES30.glGetString(GLES30.GL_VENDOR);
            glRenderer = GLES30.glGetString(GLES30.GL_RENDERER);
            int major = parseGlesMajor(glVersion);
            if (major < REQUESTED_GLES_MAJOR) {
                fail("requested_gles_3_context_unavailable");
                return;
            }
            try {
                program = createProgram(VERTEX_SHADER, FRAGMENT_SHADER);
                positionLocation = GLES30.glGetAttribLocation(program, "aPosition");
                viewportScaleLocation = GLES30.glGetUniformLocation(program, "uViewportScale");
                angleLocation = GLES30.glGetUniformLocation(program, "uAngleRadians");
                rotateLocation = GLES30.glGetUniformLocation(program, "uRotate");
                colorLocation = GLES30.glGetUniformLocation(program, "uColor");
                for (int index = 0; index < radii.length; index++) {
                    ringVertices[index] = createRing(radii[index], RING_STROKE);
                }
                GLES30.glGenBuffers(buffers.length, buffers, 0);
                for (int index = 0; index < buffers.length; index++) {
                    float[] vertices = index < 3 ? ringVertices[index] : squareVertices;
                    FloatBuffer data = ByteBuffer.allocateDirect(vertices.length * Float.BYTES)
                            .order(ByteOrder.nativeOrder()).asFloatBuffer();
                    data.put(vertices).position(0);
                    GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, buffers[index]);
                    GLES30.glBufferData(GLES30.GL_ARRAY_BUFFER, vertices.length * Float.BYTES,
                            data, GLES30.GL_STATIC_DRAW);
                }
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, 0);
                if (!checkGl("renderer initialization")) {
                    return;
                }
                recordEvent("gl_context_created", "\"gles_version\":\"" + escape(glVersion) + "\"");
            } catch (RuntimeException exception) {
                fail("renderer_initialization_failed:" + exception.getMessage());
            }
        }

        @Override
        public void onSurfaceChanged(GL10 ignored, int surfaceWidth, int surfaceHeight) {
            if (surfaceWidth <= 0 || surfaceHeight <= 0 || terminal.get()) {
                return;
            }
            width = surfaceWidth;
            height = surfaceHeight;
            float shortSide = Math.min(width, height);
            viewportScaleX = shortSide / width;
            viewportScaleY = shortSide / height;
            GLES30.glViewport(0, 0, width, height);
            Display.Mode mode = getWindowManager().getDefaultDisplay().getMode();
            refreshHz = mode.getRefreshRate();
            recordEvent("surface_changed", "\"width_px\":" + width + ",\"height_px\":" + height);
            profileConforming = isProfileConforming();
        }

        @Override
        public void onDrawFrame(GL10 ignored) {
            if (terminal.get() || program == 0 || width <= 0 || height <= 0) {
                return;
            }
            long callbackStartNs = SystemClock.elapsedRealtimeNanos();
            if (!sceneReadyRecorded) {
                renderFrame(0.0f, false);
                if (terminal.get()) {
                    return;
                }
                sceneReadyRecorded = true;
                sceneReadyNs = SystemClock.elapsedRealtimeNanos();
                recordEvent("scene_ready", "");
                profileConforming = isProfileConforming();
                return;
            }
            long warmupEndNs = sceneReadyNs + (long) warmupSeconds * NANOS_PER_SECOND;
            long captureEndNs = warmupEndNs + (long) durationSeconds * NANOS_PER_SECOND;
            long elapsedNs = callbackStartNs - sceneReadyNs;
            if (callbackStartNs < warmupEndNs) {
                float warmupAngle = (float) -Math.toRadians(
                        (elapsedNs / (double) NANOS_PER_SECOND * DEGREES_PER_SECOND) % 360.0);
                renderFrame(warmupAngle, false);
                return;
            }
            if (!measurementStarted) {
                measurementStarted = true;
                measurementStartNs = callbackStartNs;
                lastMeasuredCallbackNs = -1;
                recordEvent("measurement_started", "");
            }
            if (callbackStartNs >= captureEndNs) {
                measurementEndNs = callbackStartNs;
                recordEvent("measurement_ended", "");
                finishRun("completed", null);
                return;
            }

            long updateStartNs = SystemClock.elapsedRealtimeNanos();
            float angleRadians = (float) -Math.toRadians(
                    (elapsedNs / (double) NANOS_PER_SECOND * DEGREES_PER_SECOND) % 360.0);
            long updateDurationNs = Math.max(0L, SystemClock.elapsedRealtimeNanos() - updateStartNs);
            long submissionStartNs = SystemClock.elapsedRealtimeNanos();
            renderFrame(angleRadians, true);
            long submissionDurationNs = Math.max(0L,
                    SystemClock.elapsedRealtimeNanos() - submissionStartNs);
            if ((frameSamples % 60) == 0) {
                checkGl("sampled render");
            }
            if (terminal.get()) {
                return;
            }
            if (terminal.get()) {
                return;
            }
            long callbackIntervalNs = lastMeasuredCallbackNs < 0
                    ? -1 : callbackStartNs - lastMeasuredCallbackNs;
            lastMeasuredCallbackNs = callbackStartNs;
            frameSamples++;
            recordEvent("frame", metricsEvent(updateDurationNs, submissionDurationNs, callbackIntervalNs));
        }

        private boolean isProfileConforming() {
            return width == REQUESTED_WIDTH && height == REQUESTED_HEIGHT
                    && getResources().getConfiguration().orientation == Configuration.ORIENTATION_PORTRAIT
                    && parseGlesMajor(glVersion) == REQUESTED_GLES_MAJOR
                    && Float.compare(refreshHz, REQUESTED_REFRESH_HZ) == 0;
        }

        private String observedProfileJson() {
            int orientationValue = getResources().getConfiguration().orientation;
            String orientation = orientationValue == Configuration.ORIENTATION_PORTRAIT ? "portrait"
                    : orientationValue == Configuration.ORIENTATION_LANDSCAPE ? "landscape" : null;
            return "{"
                    + "\"surface_width_px\":" + evidenceNumber("GLSurfaceView.Renderer.onSurfaceChanged", Integer.toString(width)) + ","
                    + "\"surface_height_px\":" + evidenceNumber("GLSurfaceView.Renderer.onSurfaceChanged", Integer.toString(height)) + ","
                    + "\"orientation\":" + evidenceString("Android Configuration.orientation", orientation) + ","
                    + "\"gles_major\":" + evidenceNumber("GL_VERSION parsed from glGetString", Integer.toString(parseGlesMajor(glVersion))) + ","
                    + "\"gles_version\":" + evidenceString("glGetString(GL_VERSION)", glVersion) + ","
                    + "\"gl_vendor\":" + evidenceString("glGetString(GL_VENDOR)", glVendor) + ","
                    + "\"gl_renderer\":" + evidenceString("glGetString(GL_RENDERER)", glRenderer) + ","
                    + "\"display_refresh_hz\":" + evidenceNumber("Display.Mode.getRefreshRate; mode capability, not presentation cadence", format(refreshHz))
                    + "}";
        }

        private String errorsJson() {
            StringBuilder result = new StringBuilder("[");
            for (int index = 0; index < errors.size(); index++) {
                if (index > 0) {
                    result.append(',');
                }
                result.append('"').append(escape(errors.get(index))).append('"');
            }
            return result.append(']').toString();
        }

        private void renderFrame(float angleRadians, boolean timedFrame) {
            GLES30.glClear(GLES30.GL_COLOR_BUFFER_BIT);
            GLES30.glUseProgram(program);
            GLES30.glUniform2f(viewportScaleLocation, viewportScaleX, viewportScaleY);
            GLES30.glEnableVertexAttribArray(positionLocation);
            GLES30.glUniform1i(rotateLocation, 0);
            GLES30.glUniform4f(colorLocation, 1.0f, 1.0f, 1.0f, 1.0f);
            for (int index = 0; index < 3; index++) {
                GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, buffers[index]);
                GLES30.glVertexAttribPointer(positionLocation, 2, GLES30.GL_FLOAT, false, 0, 0);
                GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, SEGMENTS * 2 + 2);
            }
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, buffers[3]);
            GLES30.glVertexAttribPointer(positionLocation, 2, GLES30.GL_FLOAT, false, 0, 0);
            GLES30.glUniform1i(rotateLocation, 1);
            GLES30.glUniform1f(angleLocation, angleRadians);
            GLES30.glUniform4f(colorLocation, 1.0f, 128.0f / 255.0f, 0.0f, 1.0f);
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 6);
            GLES30.glDisableVertexAttribArray(positionLocation);
            GLES30.glBindBuffer(GLES30.GL_ARRAY_BUFFER, 0);
            GLES30.glUseProgram(0);
            if (!timedFrame) {
                checkGl("untimed render");
            }
        }

        private void fail(String reason) {
            failureReason = reason;
            errors.add(reason);
            recordEvent("renderer_error", "\"reason\":\"" + escape(reason) + "\"");
            finishRun("failed", reason);
        }

        private boolean checkGl(String stage) {
            int error = GLES30.glGetError();
            if (error != GLES30.GL_NO_ERROR) {
                fail(stage + "_gl_error_0x" + Integer.toHexString(error));
                return false;
            }
            return true;
        }

        private int createProgram(String vertexSource, String fragmentSource) {
            int vertex = compileShader(GLES30.GL_VERTEX_SHADER, vertexSource);
            int fragment = compileShader(GLES30.GL_FRAGMENT_SHADER, fragmentSource);
            int result = GLES30.glCreateProgram();
            GLES30.glAttachShader(result, vertex);
            GLES30.glAttachShader(result, fragment);
            GLES30.glLinkProgram(result);
            int[] status = new int[1];
            GLES30.glGetProgramiv(result, GLES30.GL_LINK_STATUS, status, 0);
            String log = GLES30.glGetProgramInfoLog(result);
            GLES30.glDeleteShader(vertex);
            GLES30.glDeleteShader(fragment);
            if (status[0] == 0) {
                GLES30.glDeleteProgram(result);
                throw new IllegalStateException("Shader program link failed: " + log);
            }
            return result;
        }

        private int compileShader(int type, String source) {
            int shader = GLES30.glCreateShader(type);
            GLES30.glShaderSource(shader, source);
            GLES30.glCompileShader(shader);
            int[] status = new int[1];
            GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, status, 0);
            String log = GLES30.glGetShaderInfoLog(shader);
            if (status[0] == 0) {
                GLES30.glDeleteShader(shader);
                throw new IllegalStateException("Shader compile failed: " + log);
            }
            return shader;
        }

        private int parseGlesMajor(String version) {
            if (version == null) {
                return 0;
            }
            java.util.regex.Matcher matcher = java.util.regex.Pattern
                    .compile("OpenGL ES (?:[A-Z]+ )?(\\d+)").matcher(version);
            return matcher.find() ? Integer.parseInt(matcher.group(1)) : 0;
        }

        private float[] createRing(float radius, float stroke) {
            float[] vertices = new float[(SEGMENTS + 1) * 4];
            float inner = radius - stroke / 2.0f;
            float outer = radius + stroke / 2.0f;
            for (int index = 0; index <= SEGMENTS; index++) {
                double angle = index * (Math.PI * 2.0) / SEGMENTS;
                int offset = index * 4;
                vertices[offset] = (float) Math.cos(angle) * outer;
                vertices[offset + 1] = (float) Math.sin(angle) * outer;
                vertices[offset + 2] = (float) Math.cos(angle) * inner;
                vertices[offset + 3] = (float) Math.sin(angle) * inner;
            }
            return vertices;
        }
    }
}
