package com.fang.perfprobe;

import android.app.Activity;
import android.opengl.GLES30;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.WindowManager;

import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.FloatBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicBoolean;

public final class GlesProbeActivity extends Activity {
    private static final String TAG = "FangGlesProbe";
    private static final long NANOS_PER_MILLISECOND = 1_000_000L;
    private static final long NANOS_PER_SECOND = 1_000_000_000L;

    private final List<FrameSample> samples = new ArrayList<>();
    private final AtomicBoolean finished = new AtomicBoolean();
    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    private long startNs;
    private long warmupNs;
    private long durationNs;
    private int iterations;
    private float refreshRateHz;
    private String runId;
    private GLSurfaceView surface;
    private GlesRenderer renderer;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        iterations = clamp(getIntent().getIntExtra("iterations", 8), 1, 64);
        int durationSeconds = clamp(getIntent().getIntExtra("durationSeconds", 30), 5, 120);
        int warmupSeconds = clamp(getIntent().getIntExtra("warmupSeconds", 5), 1, 30);
        runId = getIntent().getStringExtra("runId");
        if (runId == null || runId.isEmpty()) {
            runId = "manual";
        }
        durationNs = durationSeconds * NANOS_PER_SECOND;
        warmupNs = warmupSeconds * NANOS_PER_SECOND;
        refreshRateHz = getWindowManager().getDefaultDisplay().getRefreshRate();

        surface = new GLSurfaceView(this);
        surface.setEGLContextClientVersion(3);
        surface.setPreserveEGLContextOnPause(true);
        renderer = new GlesRenderer(this, iterations);
        surface.setRenderer(renderer);
        surface.setRenderMode(GLSurfaceView.RENDERMODE_CONTINUOUSLY);
        setContentView(surface);

        startNs = SystemClock.elapsedRealtimeNanos();
        Log.i(TAG, String.format(Locale.US,
            "START run_id=%s api=%d duration_seconds=%d warmup_seconds=%d iterations=%d refresh_hz=%.2f",
            runId, android.os.Build.VERSION.SDK_INT, durationSeconds, warmupSeconds, iterations, refreshRateHz));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
    }

    @Override
    protected void onPause() {
        if (surface != null) {
            surface.onPause();
        }
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (surface != null) {
            surface.onResume();
        }
    }

    private boolean recordRenderFrame(long frameEndNs, long drawDurationNs) {
        long elapsedNs = frameEndNs - startNs;
        if (elapsedNs >= warmupNs && elapsedNs < warmupNs + durationNs) {
            synchronized (samples) {
                samples.add(new FrameSample(elapsedNs, drawDurationNs));
            }
        }

        if (elapsedNs >= warmupNs + durationNs && finished.compareAndSet(false, true)) {
            mainHandler.post(() -> {
                surface.setRenderMode(GLSurfaceView.RENDERMODE_WHEN_DIRTY);
                writeResults();
                mainHandler.postDelayed(this::finish, 500);
            });
            return true;
        }
        return finished.get();
    }

    private void writeResults() {
        List<FrameSample> frameSnapshot;
        synchronized (samples) {
            frameSnapshot = new ArrayList<>(samples);
        }
        if (frameSnapshot.isEmpty()) {
            Log.e(TAG, "RESULT no GL render samples were captured");
            return;
        }

        List<Long> drawDurations = new ArrayList<>(frameSnapshot.size());
        int overBudget = 0;
        long frameBudgetNs = refreshRateHz > 0.0f
                ? (long) (NANOS_PER_SECOND / refreshRateHz)
                : (long) (NANOS_PER_SECOND / 60.0f);
        for (FrameSample sample : frameSnapshot) {
            drawDurations.add(sample.drawDurationNs);
            if (sample.drawDurationNs > frameBudgetNs) {
                overBudget++;
            }
        }

        String rendererName = renderer.rendererName == null ? "unknown" : renderer.rendererName;
        String glVersion = renderer.glVersion == null ? "unknown" : renderer.glVersion;
        long elapsedMeasuredNs = durationNs;
        String summary = String.format(Locale.US,
            "{\"api\":%d,\"iterations\":%d,\"refresh_hz\":%.2f,\"renderer\":\"%s\",\"gl_version\":\"%s\",\"frames\":%d,\"draw_ms_p50\":%.3f,\"draw_ms_p95\":%.3f,\"draw_ms_p99\":%.3f,\"over_budget_percent\":%.3f,\"measurement_seconds\":%.1f}",
                android.os.Build.VERSION.SDK_INT, iterations, refreshRateHz,
                jsonEscape(rendererName), jsonEscape(glVersion), frameSnapshot.size(),
            percentile(drawDurations, 0.50) / (double) NANOS_PER_MILLISECOND,
            percentile(drawDurations, 0.95) / (double) NANOS_PER_MILLISECOND,
            percentile(drawDurations, 0.99) / (double) NANOS_PER_MILLISECOND,
                overBudget * 100.0 / frameSnapshot.size(),
                elapsedMeasuredNs / (double) NANOS_PER_SECOND);

        try (FileOutputStream output = openFileOutput("frames.csv", MODE_PRIVATE)) {
            output.write("elapsed_ms,draw_completion_ms\n".getBytes(StandardCharsets.UTF_8));
            for (FrameSample sample : frameSnapshot) {
            String row = String.format(Locale.US, "%.3f,%.3f\n",
                        sample.elapsedNs / (double) NANOS_PER_MILLISECOND,
                sample.drawDurationNs / (double) NANOS_PER_MILLISECOND);
                output.write(row.getBytes(StandardCharsets.UTF_8));
            }
            output.write(("SUMMARY," + summary + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Exception exception) {
            Log.e(TAG, "Could not write frame samples", exception);
        }
        Log.i(TAG, "RUN_RESULT run_id=" + runId + " " + summary);
        Log.i(TAG, "RESULT " + summary);
    }

    private static long percentile(List<Long> values, double percentile) {
        if (values.isEmpty()) {
            return 0L;
        }
        List<Long> sorted = new ArrayList<>(values);
        Collections.sort(sorted);
        int rank = (int) Math.ceil(percentile * sorted.size());
        return sorted.get(Math.max(0, rank - 1));
    }

    private static int clamp(int value, int minimum, int maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    private static String jsonEscape(String value) {
        return value.replace("\\", "\\\\").replace("\"", "\\\"");
    }

    private static int compileShader(int type, String source) {
        int shader = GLES30.glCreateShader(type);
        GLES30.glShaderSource(shader, source);
        GLES30.glCompileShader(shader);
        int[] status = new int[1];
        GLES30.glGetShaderiv(shader, GLES30.GL_COMPILE_STATUS, status, 0);
        if (status[0] == 0) {
            String message = GLES30.glGetShaderInfoLog(shader);
            GLES30.glDeleteShader(shader);
            throw new IllegalStateException("Shader compilation failed: " + message);
        }
        return shader;
    }

    private static final class FrameSample {
        final long elapsedNs;
        final long drawDurationNs;

        FrameSample(long elapsedNs, long drawDurationNs) {
            this.elapsedNs = elapsedNs;
            this.drawDurationNs = drawDurationNs;
        }
    }

    private static final class GlesRenderer implements GLSurfaceView.Renderer {
        private static final String VERTEX_SHADER =
                "#version 300 es\n" +
                "in vec2 aPosition;\n" +
                "out vec2 vUv;\n" +
                "void main(){vUv=aPosition*0.5+0.5;gl_Position=vec4(aPosition,0.0,1.0);}\n";
        private static final String FRAGMENT_SHADER =
                "#version 300 es\n" +
                "precision highp float;\n" +
                "in vec2 vUv;\n" +
                "uniform float uTime;\n" +
                "uniform int uIterations;\n" +
                "out vec4 fragColor;\n" +
                "void main(){\n" +
                "  vec2 p=(vUv*2.0-1.0)*vec2(0.75,1.0);\n" +
                "  vec2 z=p; float value=0.0;\n" +
                "  for(int i=0;i<64;i++){\n" +
                "    if(i>=uIterations) break;\n" +
                "    z=vec2(z.x*z.x-z.y*z.y,2.0*z.x*z.y)+p*0.015;\n" +
                "    value+=sin(dot(z,z)+uTime*0.001+float(i));\n" +
                "  }\n" +
                "  fragColor=vec4(0.5+0.5*cos(value+vec3(0.0,2.0,4.0)),1.0);\n" +
                "}\n";

        private final GlesProbeActivity activity;
        private final int iterations;
        private final FloatBuffer vertices;
        private int program;
        private int positionLocation;
        private int timeLocation;
        private int iterationsLocation;
        volatile String rendererName;
        volatile String glVersion;

        GlesRenderer(GlesProbeActivity activity, int iterations) {
            this.activity = activity;
            this.iterations = iterations;
            vertices = ByteBuffer.allocateDirect(6 * Float.BYTES)
                    .order(ByteOrder.nativeOrder()).asFloatBuffer();
            vertices.put(new float[]{-1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f}).position(0);
        }

        @Override
        public void onSurfaceCreated(javax.microedition.khronos.opengles.GL10 gl,
                                     javax.microedition.khronos.egl.EGLConfig config) {
            rendererName = GLES30.glGetString(GLES30.GL_RENDERER);
            glVersion = GLES30.glGetString(GLES30.GL_VERSION);
            int vertexShader = compileShader(GLES30.GL_VERTEX_SHADER, VERTEX_SHADER);
            int fragmentShader = compileShader(GLES30.GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
            program = GLES30.glCreateProgram();
            GLES30.glAttachShader(program, vertexShader);
            GLES30.glAttachShader(program, fragmentShader);
            GLES30.glLinkProgram(program);
            int[] status = new int[1];
            GLES30.glGetProgramiv(program, GLES30.GL_LINK_STATUS, status, 0);
            GLES30.glDeleteShader(vertexShader);
            GLES30.glDeleteShader(fragmentShader);
            if (status[0] == 0) {
                throw new IllegalStateException("Program link failed: " + GLES30.glGetProgramInfoLog(program));
            }
            positionLocation = GLES30.glGetAttribLocation(program, "aPosition");
            timeLocation = GLES30.glGetUniformLocation(program, "uTime");
            iterationsLocation = GLES30.glGetUniformLocation(program, "uIterations");
        }

        @Override
        public void onSurfaceChanged(javax.microedition.khronos.opengles.GL10 gl, int width, int height) {
            GLES30.glViewport(0, 0, width, height);
        }

        @Override
        public void onDrawFrame(javax.microedition.khronos.opengles.GL10 gl) {
            if (activity.finished.get()) {
                return;
            }
            long frameStartNs = SystemClock.elapsedRealtimeNanos();
            GLES30.glUseProgram(program);
            GLES30.glUniform1f(timeLocation, SystemClock.elapsedRealtimeNanos() / 1_000_000.0f);
            GLES30.glUniform1i(iterationsLocation, iterations);
            vertices.position(0);
            GLES30.glEnableVertexAttribArray(positionLocation);
            GLES30.glVertexAttribPointer(positionLocation, 2, GLES30.GL_FLOAT, false, 0, vertices);
            GLES30.glDrawArrays(GLES30.GL_TRIANGLES, 0, 3);
            GLES30.glDisableVertexAttribArray(positionLocation);
            GLES30.glFinish();
            long frameEndNs = SystemClock.elapsedRealtimeNanos();
            activity.recordRenderFrame(frameEndNs, frameEndNs - frameStartNs);
        }
    }
}