package com.antigravity.keyboardslm.app

import android.annotation.SuppressLint
import android.content.Context
import android.os.Build
import android.view.WindowInsets
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.inputmethodservice.InputMethodService
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputConnection
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import java.io.File
import kotlin.math.abs

class KeyboardService : InputMethodService() {

    private lateinit var engine: KeyboardEngineJni
    private lateinit var modelFile: File

    private var keyboardLayout: LinearLayout? = null
    private var candidateBar: LinearLayout? = null
    private val candidateViews = mutableListOf<TextView>()

    private var isShifted = false
    private var isCapsLock = false
    private var isSymbolsMode = false
    private var lastShiftPressTime = 0L

    private var isIncognitoOrPassword = false
    private var currentTypedSentence = StringBuilder()

    // Spacebar drag for cursor movement
    private var spacebarStartX = 0f
    private var isDraggingSpacebar = false

    override fun onCreate() {
        super.onCreate()
        modelFile = File(filesDir, "personal_model.bin")
        engine = KeyboardEngineJni.getSharedInstance(modelFile.absolutePath)
    }

    override fun onDestroy() {
        super.onDestroy()
        engine.save(modelFile.absolutePath)
    }

    override fun onStartInputView(info: EditorInfo?, restarting: Boolean) {
        super.onStartInputView(info, restarting)

        // Check if the current field is a password or sensitive field
        val variation = info?.inputType?.and(InputType.TYPE_MASK_VARIATION) ?: 0
        isIncognitoOrPassword = variation == InputType.TYPE_TEXT_VARIATION_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD ||
                variation == InputType.TYPE_TEXT_VARIATION_WEB_PASSWORD

        isSymbolsMode = false
        isShifted = false
        currentTypedSentence.clear()

        updateKeyboardLayout()
        updatePredictions()
    }

    override fun onFinishInputView(finishingInput: Boolean) {
        super.onFinishInputView(finishingInput)
        // Commit current sentence to training on close if not empty
        if (!isIncognitoOrPassword && currentTypedSentence.isNotBlank()) {
            engine.learnSentence(currentTypedSentence.toString())
            currentTypedSentence.clear()
            engine.save(modelFile.absolutePath)
        }
    }

    override fun onCreateInputView(): View {
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.parseColor("#0d131f"))
            setPadding(0, dp(4), 0, dp(6))
        }
        root.setOnApplyWindowInsetsListener { v, insets ->
            val navBottom = if (Build.VERSION.SDK_INT >= 30) {
                insets.getInsets(WindowInsets.Type.navigationBars()).bottom
            } else {
                @Suppress("DEPRECATION") insets.systemWindowInsetBottom
            }
            v.setPadding(0, dp(4), 0, dp(6) + navBottom)
            insets
        }

        // 1. Candidate Bar (Top 3 Suggestions)
        candidateBar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                dp(45)
            ).apply {
                setMargins(dp(6), 0, dp(6), 0)
            }
        }
        candidateViews.clear()
        for (i in 0 until 3) {
            val tv = createCandidatePill(i)
            candidateViews.add(tv)
            candidateBar?.addView(tv)
        }
        root.addView(candidateBar)

        // 2. Keyboard Keys Container
        keyboardLayout = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT
            )
        }
        root.addView(keyboardLayout)

        updateKeyboardLayout()
        return root
    }

    private fun createCandidatePill(index: Int): TextView {
        val pill = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, dp(38), 1f).apply {
                if (index > 0) marginStart = dp(6)
            }
            gravity = Gravity.CENTER
            textSize = 14f
            setTypeface(Typeface.DEFAULT, Typeface.BOLD)
            setTextColor(Color.WHITE)
            background = createPillBackground(Color.parseColor("#182235"))
            isClickable = true
            isFocusable = true
        }

        pill.setOnClickListener {
            val word = pill.text?.toString()
            if (!word.isNullOrEmpty()) {
                vibrateKey()
                commitSuggestion(word)
            }
        }

        pill.setOnLongClickListener {
            val word = pill.text?.toString()
            if (!word.isNullOrEmpty()) {
                vibrateKey()
                showForgetConfirmation(word)
                true
            } else {
                false
            }
        }

        return pill
    }

    private fun updateKeyboardLayout() {
        val container = keyboardLayout ?: return
        container.removeAllViews()

        val rows = if (isSymbolsMode) getSymbolsRows() else getAlphaRows()

        for ((index, rowKeys) in rows.withIndex()) {
            val isBottom = index == rows.lastIndex
            val rowLayout = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER
                layoutParams = LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    dp(if (isBottom) 56 else 52)
                ).apply {
                    setMargins(dp(3), 0, dp(3), 0)
                }
            }

            for (key in rowKeys) {
                val keyView = createKeyView(key, isBottom)
                rowLayout.addView(keyView)
            }

            container.addView(rowLayout)
        }
    }

    private val numberRow = listOf("1", "2", "3", "4", "5", "6", "7", "8", "9", "0")

    private fun getAlphaRows(): List<List<String>> {
        val r1 = listOf("q", "w", "e", "r", "t", "y", "u", "i", "o", "p")
        val r2 = listOf("a", "s", "d", "f", "g", "h", "j", "k", "l")
        val r3 = listOf("SHIFT", "z", "x", "c", "v", "b", "n", "m", "DEL")
        val r4 = listOf("?123", ",", "SPACE", ".", "ENTER")

        return if (isShifted || isCapsLock) {
            listOf(
                numberRow,
                r1.map { it.uppercase() },
                r2.map { it.uppercase() },
                r3.map { if (it == "SHIFT" || it == "DEL") it else it.uppercase() },
                r4
            )
        } else {
            listOf(numberRow, r1, r2, r3, r4)
        }
    }

    private fun getSymbolsRows(): List<List<String>> {
        return listOf(
            numberRow,
            listOf("@", "#", "$", "%", "&", "-", "+", "(", ")", "/"),
            listOf("~", "`", "|", "<", ">", "{", "}", "[", "]", "\\"),
            listOf("ABC", "*", "\"", "'", ":", ";", "!", "?", "DEL"),
            listOf("?123", ",", "SPACE", ".", "ENTER")
        )
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun createKeyView(keyText: String, isBottomRow: Boolean = false): View {
        val isSpecial = keyText == "SHIFT" || keyText == "DEL" || keyText == "?123" || keyText == "ABC" || keyText == "ENTER"
        val isSpace = keyText == "SPACE"
        val isDigit = keyText.length == 1 && keyText[0].isDigit() && !isSpecial

        val weight = when {
            isSpace -> 4.2f
            keyText == "SHIFT" || keyText == "DEL" -> 1.5f
            keyText == "ENTER" || keyText == "?123" || keyText == "ABC" -> 1.4f
            else -> 1.0f
        }

        val button = TextView(this).apply {
            layoutParams = LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.MATCH_PARENT, weight).apply {
                setMargins(dp(2), dp(if (isBottomRow) 8 else 4), dp(2), dp(if (isBottomRow) 8 else 4))
            }
            contentDescription = keyText
            gravity = Gravity.CENTER
            textSize = if (isSpecial) 13f else if (isDigit) 18f else 20f
            setTypeface(Typeface.DEFAULT, if (isSpecial) Typeface.BOLD else Typeface.NORMAL)
            setTextColor(Color.WHITE)

            val bgColor = when {
                keyText == "ENTER" -> Color.parseColor("#4f46e5")
                keyText == "SHIFT" && (isShifted || isCapsLock) -> Color.parseColor("#4f46e5")
                isSpecial -> Color.parseColor("#152033")
                else -> Color.parseColor("#1e293b")
            }
            background = createPillBackground(bgColor)

            text = when (keyText) {
                "SHIFT" -> if (isCapsLock) "⇪" else "⇧"
                "DEL" -> "⌫"
                "SPACE" -> ""
                "ENTER" -> "↵"
                else -> keyText
            }
        }

        // Special touch handling for spacebar cursor swiping
        if (isSpace) {
            button.setOnTouchListener { _, event ->
                when (event.action) {
                    MotionEvent.ACTION_DOWN -> {
                        spacebarStartX = event.x
                        isDraggingSpacebar = false
                    }
                    MotionEvent.ACTION_MOVE -> {
                        val dx = event.x - spacebarStartX
                        if (abs(dx) > dp(18)) {
                            isDraggingSpacebar = true
                            vibrateKey()
                            if (dx > 0) moveCursor(1) else moveCursor(-1)
                            spacebarStartX = event.x
                        }
                    }
                    MotionEvent.ACTION_UP -> {
                        if (!isDraggingSpacebar) {
                            handleKeyPress("SPACE")
                        }
                        isDraggingSpacebar = false
                    }
                }
                true
            }
        } else {
            button.setOnClickListener {
                handleKeyPress(keyText)
            }
        }

        return button
    }

    private fun handleKeyPress(key: String) {
        vibrateKey()
        val ic = currentInputConnection ?: return

        when (key) {
            "SHIFT" -> {
                val now = System.currentTimeMillis()
                if (now - lastShiftPressTime < 300) {
                    isCapsLock = !isCapsLock
                    isShifted = isCapsLock
                } else {
                    isShifted = !isShifted
                    if (!isShifted) isCapsLock = false
                }
                lastShiftPressTime = now
                updateKeyboardLayout()
            }
            "?123" -> {
                isSymbolsMode = true
                updateKeyboardLayout()
            }
            "ABC" -> {
                isSymbolsMode = false
                updateKeyboardLayout()
            }
            "DEL" -> {
                ic.deleteSurroundingText(1, 0)
                if (currentTypedSentence.isNotEmpty()) {
                    currentTypedSentence.deleteCharAt(currentTypedSentence.length - 1)
                }
                updatePredictions()
            }
            "SPACE" -> {
                val prevContext = extractContext()
                val currentWord = prevContext.third
                ic.commitText(" ", 1)
                currentTypedSentence.append(" ")

                // Learn committed word online
                if (!isIncognitoOrPassword && currentWord.isNotEmpty()) {
                    engine.learnWord(prevContext.first, prevContext.second, currentWord)
                }

                if (isShifted && !isCapsLock) {
                    isShifted = false
                    updateKeyboardLayout()
                }

                updatePredictions()
            }
            "ENTER" -> {
                // Send action or insert newline
                val action = currentInputEditorInfo?.actionId ?: EditorInfo.IME_ACTION_NONE
                if (action != EditorInfo.IME_ACTION_NONE) {
                    ic.performEditorAction(action)
                } else {
                    ic.commitText("\n", 1)
                }

                if (!isIncognitoOrPassword && currentTypedSentence.isNotBlank()) {
                    engine.learnSentence(currentTypedSentence.toString())
                    currentTypedSentence.clear()
                    engine.save(modelFile.absolutePath)
                }

                updatePredictions()
            }
            else -> {
                ic.commitText(key, 1)
                currentTypedSentence.append(key)

                if (isShifted && !isCapsLock) {
                    isShifted = false
                    updateKeyboardLayout()
                }

                updatePredictions()
            }
        }
    }

    private fun commitSuggestion(word: String) {
        val ic = currentInputConnection ?: return
        val context = extractContext()
        val currentPrefix = context.third

        // Delete current prefix from the input field
        if (currentPrefix.isNotEmpty()) {
            ic.deleteSurroundingText(currentPrefix.length, 0)
        }

        // Commit chosen word + trailing space
        ic.commitText("$word ", 1)
        currentTypedSentence.append("$word ")

        // Learn online
        if (!isIncognitoOrPassword) {
            engine.learnWord(context.first, context.second, word)
        }

        updatePredictions()
    }

    private fun updatePredictions() {
        val context = extractContext()
        val p2 = context.first
        val p1 = context.second
        val prefix = context.third

        val predictions = engine.predict(p2, p1, prefix, 3)

        for (i in 0 until 3) {
            val tv = candidateViews.getOrNull(i) ?: continue
            if (i < predictions.size) {
                tv.text = predictions[i].word
                tv.visibility = View.VISIBLE
            } else {
                tv.text = ""
                tv.visibility = View.INVISIBLE
            }
        }
    }

    private fun extractContext(): Triple<String, String, String> {
        val ic = currentInputConnection ?: return Triple("", "", "")
        val textBefore = ic.getTextBeforeCursor(120, 0)?.toString() ?: ""

        val isNextWord = textBefore.isNotEmpty() && textBefore.last().isWhitespace()
        val tokens = textBefore.trim().split(Regex("\\s+")).filter { it.isNotBlank() }

        return if (isNextWord || tokens.isEmpty()) {
            val p1 = tokens.lastOrNull() ?: ""
            val p2 = if (tokens.size >= 2) tokens[tokens.size - 2] else ""
            Triple(p2, p1, "")
        } else {
            val prefix = tokens.last()
            val p1 = if (tokens.size >= 2) tokens[tokens.size - 2] else ""
            val p2 = if (tokens.size >= 3) tokens[tokens.size - 3] else ""
            Triple(p2, p1, prefix)
        }
    }

    private fun moveCursor(offset: Int) {
        val ic = currentInputConnection ?: return
        if (offset > 0) {
            ic.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_DPAD_RIGHT))
            ic.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_UP, android.view.KeyEvent.KEYCODE_DPAD_RIGHT))
        } else {
            ic.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_DOWN, android.view.KeyEvent.KEYCODE_DPAD_LEFT))
            ic.sendKeyEvent(android.view.KeyEvent(android.view.KeyEvent.ACTION_UP, android.view.KeyEvent.KEYCODE_DPAD_LEFT))
        }
    }

    private fun showForgetConfirmation(word: String) {
        engine.forgetWord(word)
        engine.save(modelFile.absolutePath)
        Toast.makeText(this, "Removed \"$word\" from personal dictionary", Toast.LENGTH_SHORT).show()
        updatePredictions()
    }

    private fun vibrateKey() {
        keyboardLayout?.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP)
    }

    private fun createPillBackground(color: Int): GradientDrawable {
        return GradientDrawable().apply {
            setColor(color)
            cornerRadius = dp(8).toFloat()
        }
    }

    private fun dp(v: Int): Int {
        return TypedValue.applyDimension(
            TypedValue.COMPLEX_UNIT_DIP,
            v.toFloat(),
            resources.displayMetrics
        ).toInt()
    }
}
