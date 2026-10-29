package org.matonos.flathub.fdroid

import android.content.Context
import androidx.work.Constraints
import androidx.work.ExistingPeriodicWorkPolicy
import androidx.work.NetworkType
import androidx.work.PeriodicWorkRequestBuilder
import androidx.work.WorkManager
import androidx.work.Worker
import androidx.work.WorkerParameters
import java.util.concurrent.TimeUnit

class FdroidUpdateWorker(context: Context, params: WorkerParameters) : Worker(context, params) {
  override fun doWork(): Result = try {
    FdroidUpdateChecker.checkInBackground(applicationContext)
    Result.success()
  } catch (_: Exception) {
    Result.retry()
  }

  companion object {
    private const val UNIQUE_WORK = "fdroid-update-check"

    fun schedule(context: Context) {
      val request = PeriodicWorkRequestBuilder<FdroidUpdateWorker>(12, TimeUnit.HOURS)
        .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
        .build()
      WorkManager.getInstance(context).enqueueUniquePeriodicWork(UNIQUE_WORK, ExistingPeriodicWorkPolicy.KEEP, request)
    }
  }
}
