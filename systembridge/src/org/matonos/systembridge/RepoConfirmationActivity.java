package org.matonos.systembridge;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.view.WindowManager;

/** Non-exported system UI. Display data comes only from the bridge's request map. */
public final class RepoConfirmationActivity extends Activity {
    private String operation;
    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        getWindow().setHideOverlayWindows(true);
        operation=getIntent().getStringExtra("operation");
        SystemBridgeService.RepoRequest request=SystemBridgeService.repoRequests.get(operation);
        if(request==null || request.decision.state()!=RepoDecision.State.PENDING){finish();return;}
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle(request.removing?"Remove repository?":"Add repository?")
            .setMessage(request.message).setPositiveButton(request.removing?"Remove":"Add",(d,w)->decide(RepoDecision.State.CONFIRMED))
            .setNegativeButton("Cancel",(d,w)->decide(RepoDecision.State.DENIED))
            .setOnCancelListener(d->decide(RepoDecision.State.DENIED)).create();
        dialog.setCanceledOnTouchOutside(false);dialog.show();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setFilterTouchesWhenObscured(true);
        android.os.Handler handler=new android.os.Handler(getMainLooper());
        handler.postDelayed(new Runnable(){public void run(){
            if(isFinishing() || isDestroyed())return;
            if(!SystemBridgeService.repoRequests.containsKey(operation) || request.decision.state()!=RepoDecision.State.PENDING){finish();return;}
            handler.postDelayed(this,250);
        }},250);
        new android.os.Handler(getMainLooper()).postDelayed(()->{if(!isFinishing())decide(RepoDecision.State.TIMED_OUT);},120000);
    }
    private void decide(RepoDecision.State state) {
        SystemBridgeService bridge=SystemBridgeService.repoBridge();
        if(bridge!=null)bridge.decideRepo(operation,state);
        finish();
    }
    @Override public void onDestroy() {
        if(!isChangingConfigurations()) {
            SystemBridgeService bridge=SystemBridgeService.repoBridge();
            if(bridge!=null)bridge.decideRepo(operation,RepoDecision.State.DENIED);
        }
        super.onDestroy();
    }
}
