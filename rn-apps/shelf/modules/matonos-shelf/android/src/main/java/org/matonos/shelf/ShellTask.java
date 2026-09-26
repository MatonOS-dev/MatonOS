package org.matonos.shelf;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

final class ShellTask {
    final int taskId;
    final String packageName;
    final int windowingMode;

    ShellTask(int taskId, String packageName, int windowingMode) {
        this.taskId = taskId;
        this.packageName = packageName;
        this.windowingMode = windowingMode;
    }

    static List<ShellTask> parse(String json) throws JSONException {
        JSONArray array = new JSONArray(json);
        ArrayList<ShellTask> tasks = new ArrayList<>();
        for (int i = 0; i < array.length(); i++) {
            JSONObject item = array.getJSONObject(i);
            String pkg = item.optString("packageName", "");
            int id = item.optInt("taskId", -1);
            if (!pkg.isEmpty() && id >= 0) tasks.add(new ShellTask(id, pkg,
                    item.optInt("windowingMode", 1)));
        }
        return tasks;
    }
}
