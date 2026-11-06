package com.antigravity.keyboardslm.app

import org.json.JSONArray
import org.json.JSONObject

data class PredictionResult(
    val word: String,
    val score: Double,
    val source: String
)

data class EngineStatsResult(
    val uniqueWords: Int,
    val totalTokens: Long,
    val bigramEntries: Int,
    val trigramEntries: Int,
    val memoryKb: Double
)

class KeyboardEngineJni {
    companion object {
        init {
            System.loadLibrary("keyboard_slm")
        }

        private var instance: KeyboardEngineJni? = null

        @Synchronized
        fun getSharedInstance(modelPath: String? = null): KeyboardEngineJni {
            if (instance == null) {
                instance = KeyboardEngineJni()
                if (modelPath != null) {
                    instance!!.load(modelPath)
                }
            }
            return instance!!
        }
    }

    private var nativeHandle: Long = initEngine()

    fun isReady(): Boolean = nativeHandle != 0L

    fun close() {
        if (nativeHandle != 0L) {
            destroyEngine(nativeHandle)
            nativeHandle = 0L
        }
    }

    fun learnWord(prev2: String, prev1: String, word: String, weight: Int = 1) {
        if (nativeHandle != 0L) {
            learnWord(nativeHandle, prev2, prev1, word, weight)
        }
    }

    fun learnSentence(sentence: String, weight: Int = 1) {
        if (nativeHandle != 0L) {
            learnSentence(nativeHandle, sentence, weight)
        }
    }

    fun forgetWord(word: String): Boolean {
        return if (nativeHandle != 0L) forgetWord(nativeHandle, word) else false
    }

    fun predict(prev2: String, prev1: String, prefix: String, topK: Int = 3): List<PredictionResult> {
        if (nativeHandle == 0L) return emptyList()
        val jsonStr = predictJson(nativeHandle, prev2, prev1, prefix, topK)
        val list = mutableListOf<PredictionResult>()
        try {
            val arr = JSONArray(jsonStr)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(
                    PredictionResult(
                        word = obj.getString("word"),
                        score = obj.getDouble("score"),
                        source = obj.optString("source", "unigram")
                    )
                )
            }
        } catch (_: Exception) {}
        return list
    }

    fun listSenders(chatContent: String): List<Pair<String, Long>> {
        val jsonStr = listSendersJson(chatContent)
        val list = mutableListOf<Pair<String, Long>>()
        try {
            val arr = JSONArray(jsonStr)
            for (i in 0 until arr.length()) {
                val obj = arr.getJSONObject(i)
                list.add(Pair(obj.getString("name"), obj.getLong("count")))
            }
        } catch (_: Exception) {}
        return list
    }

    fun importChat(chatContent: String, targetSender: String): Long {
        return if (nativeHandle != 0L) importChat(nativeHandle, chatContent, targetSender) else 0L
    }

    fun getStats(): EngineStatsResult {
        if (nativeHandle == 0L) return EngineStatsResult(0, 0, 0, 0, 0.0)
        val jsonStr = getStatsJson(nativeHandle)
        return try {
            val obj = JSONObject(jsonStr)
            EngineStatsResult(
                uniqueWords = obj.optInt("unique_words", 0),
                totalTokens = obj.optLong("total_tokens", 0),
                bigramEntries = obj.optInt("bigram_entries", 0),
                trigramEntries = obj.optInt("trigram_entries", 0),
                memoryKb = obj.optDouble("memory_kb", 0.0)
            )
        } catch (_: Exception) {
            EngineStatsResult(0, 0, 0, 0, 0.0)
        }
    }

    fun save(filepath: String): Boolean {
        return if (nativeHandle != 0L) saveModel(nativeHandle, filepath) else false
    }

    fun load(filepath: String): Boolean {
        return if (nativeHandle != 0L) loadModel(nativeHandle, filepath) else false
    }

    fun clear() {
        if (nativeHandle != 0L) clearModel(nativeHandle)
    }

    // Native declarations
    private external fun initEngine(): Long
    private external fun destroyEngine(handle: Long)
    private external fun learnWord(handle: Long, prev2: String, prev1: String, word: String, weight: Int)
    private external fun learnSentence(handle: Long, sentence: String, weight: Int)
    private external fun forgetWord(handle: Long, word: String): Boolean
    private external fun predictJson(handle: Long, prev2: String, prev1: String, prefix: String, topK: Int): String
    private external fun listSendersJson(chatContent: String): String
    private external fun importChat(handle: Long, chatContent: String, targetSender: String): Long
    private external fun getStatsJson(handle: Long): String
    private external fun saveModel(handle: Long, filepath: String): Boolean
    private external fun loadModel(handle: Long, filepath: String): Boolean
    private external fun clearModel(handle: Long)
}
