package com.antigravity.keyboardslm.app

import android.content.Context
import android.content.Intent
import android.provider.Settings
import android.view.inputmethod.InputMethodManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodChannel
import java.io.File

class MainActivity : FlutterActivity() {
    private val CHANNEL = "com.antigravity.keyboardslm/bridge"

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)

        MethodChannel(flutterEngine.dartExecutor.binaryMessenger, CHANNEL).setMethodCallHandler { call, result ->
            val modelFile = File(filesDir, "personal_model.bin")
            val engine = KeyboardEngineJni.getSharedInstance(modelFile.absolutePath)

            when (call.method) {
                "isKeyboardEnabled" -> {
                    val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                    val enabledMethods = imm.enabledInputMethodList
                    val pkgName = packageName
                    val isEnabled = enabledMethods.any { it.packageName == pkgName }
                    result.success(isEnabled)
                }
                "openKeyboardSettings" -> {
                    startActivity(Intent(Settings.ACTION_INPUT_METHOD_SETTINGS))
                    result.success(true)
                }
                "openKeyboardPicker" -> {
                    val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
                    imm.showInputMethodPicker()
                    result.success(true)
                }
                "getStats" -> {
                    val stats = engine.getStats()
                    result.success(
                        mapOf(
                            "uniqueWords" to stats.uniqueWords,
                            "totalTokens" to stats.totalTokens,
                            "bigramEntries" to stats.bigramEntries,
                            "trigramEntries" to stats.trigramEntries,
                            "memoryKb" to stats.memoryKb
                        )
                    )
                }
                "listSenders" -> {
                    val content = call.argument<String>("content") ?: ""
                    val senders = engine.listSenders(content)
                    val list = senders.map { mapOf("name" to it.first, "count" to it.second) }
                    result.success(list)
                }
                "importChat" -> {
                    val content = call.argument<String>("content") ?: ""
                    val sender = call.argument<String>("sender") ?: ""
                    val imported = engine.importChat(content, sender)
                    engine.save(modelFile.absolutePath)
                    result.success(imported)
                }
                "clearModel" -> {
                    engine.clear()
                    engine.save(modelFile.absolutePath)
                    result.success(true)
                }
                "predict" -> {
                    val p2 = call.argument<String>("prev2") ?: ""
                    val p1 = call.argument<String>("prev1") ?: ""
                    val prefix = call.argument<String>("prefix") ?: ""
                    val topK = call.argument<Int>("topK") ?: 3
                    val preds = engine.predict(p2, p1, prefix, topK)
                    val list = preds.map {
                        mapOf("word" to it.word, "score" to it.score, "source" to it.source)
                    }
                    result.success(list)
                }
                else -> {
                    result.notImplemented()
                }
            }
        }
    }
}
