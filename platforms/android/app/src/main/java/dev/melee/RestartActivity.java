package dev.melee;

/* Options' Restart now (MeleeActivity.restartGame): "Restarting Xelee" for
 * the moment between the game's process ending and the game starting again.
 * In a process of its own (":restart"), so it outlives the game's; it waits
 * for that one to exit by itself, stops it after two seconds if it hasn't,
 * then starts the game's activity with the disc it was playing, which skips
 * the launcher (MeleeActivity.getArguments). */
public class RestartActivity extends android.app.Activity {
    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        android.widget.TextView text = new android.widget.TextView(this);
        text.setText("Restarting Xelee…");
        text.setTextSize(24);
        text.setTextColor(0xfff0e4b8);
        text.setBackgroundColor(0xff0c0b08);
        text.setGravity(android.view.Gravity.CENTER);
        setContentView(text);

        final int pid = getIntent().getIntExtra("pid", 0);
        final String disc = getIntent().getStringExtra("disc");
        final boolean xr = getIntent().getBooleanExtra("xr", false);
        new Thread(new Runnable() {
            @Override
            public void run() {
                for (int i = 0; alive(pid) && i < 300; i++) {
                    if (i == 100)
                        android.os.Process.killProcess(pid);
                    pause(20);
                }
                pause(150); // for the system to see the old process gone
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        android.content.Intent game = new android.content.Intent(RestartActivity.this,
                            xr ? MeleeXrActivity.class : MeleeActivity.class);
                        game.setAction(android.content.Intent.ACTION_MAIN);
                        game.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK);
                        if (disc != null && !disc.isEmpty())
                            game.putExtra("disc", disc);
                        startActivity(game);
                        finishAndRemoveTask();
                    }
                });
            }
        }).start();
    }

    private static boolean alive(int pid) {
        return pid > 0 && new java.io.File("/proc/" + pid).exists();
    }

    private static void pause(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }
}
