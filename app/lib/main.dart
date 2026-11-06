import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

void main() {
  runApp(const KeyboardSLMApp());
}

class KeyboardSLMApp extends StatelessWidget {
  const KeyboardSLMApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Keyboard SLM',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      darkTheme: ThemeData(
        useMaterial3: true,
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF090D16),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFF6366F1),
          surface: Color(0xFF111827),
          onSurface: Color(0xFFF3F4F6),
          outline: Color(0xFF1F293D),
        ),
        cardTheme: CardTheme(
          color: const Color(0xFF111827),
          elevation: 0,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(16),
            side: const BorderSide(color: Color(0xFF1F293D)),
          ),
        ),
      ),
      home: const DashboardScreen(),
    );
  }
}

class DashboardScreen extends StatefulWidget {
  const DashboardScreen({super.key});

  @override
  State<DashboardScreen> createState() => _DashboardScreenState();
}

class _DashboardScreenState extends State<DashboardScreen> {
  static const platform = MethodChannel('com.antigravity.keyboardslm/bridge');

  bool _isKeyboardEnabled = false;
  Map<String, dynamic> _stats = {
    'uniqueWords': 0,
    'totalTokens': 0,
    'bigramEntries': 0,
    'trigramEntries': 0,
    'memoryKb': 0.0,
  };
  bool _isLoading = false;

  final TextEditingController _testInputController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _refreshStatusAndStats();
  }

  Future<void> _refreshStatusAndStats() async {
    setState(() => _isLoading = true);
    try {
      final bool enabled = await platform.invokeMethod('isKeyboardEnabled') ?? false;
      final Map<dynamic, dynamic>? statsRes = await platform.invokeMethod('getStats');
      if (mounted) {
        setState(() {
          _isKeyboardEnabled = enabled;
          if (statsRes != null) {
            _stats = Map<String, dynamic>.from(statsRes);
          }
          _isLoading = false;
        });
      }
    } catch (_) {
      if (mounted) setState(() => _isLoading = false);
    }
  }

  Future<void> _openSettings() async {
    await platform.invokeMethod('openKeyboardSettings');
  }

  Future<void> _openPicker() async {
    await platform.invokeMethod('openKeyboardPicker');
  }

  Future<void> _clearModel() async {
    final bool? confirm = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Reset Vocabulary?'),
        content: const Text('This will erase all learned words and transitions, returning the keyboard to factory state.'),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('Cancel')),
          FilledButton(
            style: FilledButton.styleFrom(backgroundColor: Colors.red),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Reset'),
          ),
        ],
      ),
    );

    if (confirm == true) {
      await platform.invokeMethod('clearModel');
      await _refreshStatusAndStats();
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('Model vocabulary reset successfully')),
        );
      }
    }
  }

  void _showImportDialog() {
    final TextEditingController textController = TextEditingController();
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Import WhatsApp Chat'),
        content: SizedBox(
          width: double.maxFinite,
          child: Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                'Paste the contents of your exported _chat.txt below:',
                style: TextStyle(fontSize: 13, color: Colors.white70),
              ),
              const SizedBox(height: 12),
              TextField(
                controller: textController,
                maxLines: 8,
                decoration: const InputDecoration(
                  hintText: '[16/10/26, 9:12 PM] Vikas: bro em chesthunav...',
                  border: OutlineInputBorder(),
                  contentPadding: EdgeInsets.all(12),
                ),
              ),
            ],
          ),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          FilledButton(
            onPressed: () async {
              final content = textController.text.trim();
              if (content.isEmpty) return;
              Navigator.pop(ctx);
              _inspectAndImportChat(content);
            },
            child: const Text('Next'),
          ),
        ],
      ),
    );
  }

  Future<void> _inspectAndImportChat(String content) async {
    try {
      final List<dynamic>? sendersRes = await platform.invokeMethod('listSenders', {'content': content});
      if (sendersRes == null || sendersRes.isEmpty) {
        if (!mounted) return;
        ScaffoldMessenger.of(context).showSnackBar(
          const SnackBar(content: Text('No valid messages found in text')),
        );
        return;
      }

      String? selectedSender = sendersRes.first['name'] as String?;

      if (!mounted) return;
      showDialog(
        context: context,
        builder: (ctx) => StatefulBuilder(
          builder: (ctx, setDialogState) => AlertDialog(
            title: const Text('Select Your Name'),
            content: SizedBox(
              width: double.maxFinite,
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  const Text('Pick which sender messages to learn from:', style: TextStyle(fontSize: 13, color: Colors.white70)),
                  const SizedBox(height: 12),
                  ...sendersRes.map((s) {
                    final name = s['name'] as String;
                    final count = s['count'] as int;
                    return RadioListTile<String>(
                      title: Text(name, style: const TextStyle(fontWeight: FontWeight.bold)),
                      subtitle: Text('$count messages'),
                      value: name,
                      groupValue: selectedSender,
                      onChanged: (val) {
                        setDialogState(() => selectedSender = val);
                      },
                    );
                  }),
                ],
              ),
            ),
            actions: [
              TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
              FilledButton(
                onPressed: () async {
                  Navigator.pop(ctx);
                  setState(() => _isLoading = true);
                  final imported = await platform.invokeMethod('importChat', {
                    'content': content,
                    'sender': selectedSender ?? '',
                  });
                  await _refreshStatusAndStats();
                  if (mounted) {
                    ScaffoldMessenger.of(context).showSnackBar(
                      SnackBar(content: Text('Successfully imported $imported messages from $selectedSender!')),
                    );
                  }
                },
                child: const Text('Import & Train'),
              ),
            ],
          ),
        ),
      );
    } catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(content: Text('Import error: $e')),
        );
      }
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Row(
          children: [
            CircleAvatar(
              radius: 16,
              backgroundColor: Color(0xFF6366F1),
              child: Text('K', style: TextStyle(color: Colors.white, fontWeight: FontWeight.bold)),
            ),
            SizedBox(width: 10),
            Text('Keyboard SLM', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 18)),
          ],
        ),
        actions: [
          IconButton(
            icon: const Icon(Icons.refresh),
            onPressed: _refreshStatusAndStats,
            tooltip: 'Refresh Stats',
          ),
        ],
        elevation: 0,
        backgroundColor: Colors.transparent,
      ),
      body: _isLoading
          ? const Center(child: CircularProgressIndicator())
          : RefreshIndicator(
              onRefresh: _refreshStatusAndStats,
              child: ListView(
                padding: const EdgeInsets.all(16),
                children: [
                  // Status Banner Card
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Row(
                        children: [
                          Icon(
                            _isKeyboardEnabled ? Icons.check_circle : Icons.warning_amber_rounded,
                            color: _isKeyboardEnabled ? Colors.greenAccent : Colors.amberAccent,
                            size: 32,
                          ),
                          const SizedBox(width: 14),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Text(
                                  _isKeyboardEnabled ? 'Keyboard Activated' : 'Activation Required',
                                  style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 16),
                                ),
                                const SizedBox(height: 2),
                                Text(
                                  _isKeyboardEnabled
                                      ? 'System IME is enabled and ready to use in any app.'
                                      : 'Enable Keyboard SLM in Android Settings to start using it.',
                                  style: const TextStyle(fontSize: 12, color: Colors.white70),
                                ),
                              ],
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),

                  const SizedBox(height: 12),

                  // Quick Action Buttons
                  Row(
                    children: [
                      Expanded(
                        child: FilledButton.tonalIcon(
                          icon: const Icon(Icons.settings, size: 18),
                          label: const Text('1. Enable IME'),
                          onPressed: _openSettings,
                        ),
                      ),
                      const SizedBox(width: 10),
                      Expanded(
                        child: FilledButton.icon(
                          icon: const Icon(Icons.keyboard, size: 18),
                          label: const Text('2. Switch Input'),
                          onPressed: _openPicker,
                        ),
                      ),
                    ],
                  ),

                  const SizedBox(height: 20),

                  // In-App Test Playground
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text(
                            'Interactive Test Box',
                            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 15),
                          ),
                          const SizedBox(height: 6),
                          const Text(
                            'Tap inside the field below to pop up the native Keyboard SLM and test typing live:',
                            style: TextStyle(fontSize: 12, color: Colors.white70),
                          ),
                          const SizedBox(height: 12),
                          TextField(
                            controller: _testInputController,
                            decoration: InputDecoration(
                              hintText: 'Type romanized sentences here (e.g. "bro em ")...',
                              filled: true,
                              fillColor: const Color(0xFF0B111E),
                              border: OutlineInputBorder(
                                borderRadius: BorderRadius.circular(12),
                                borderSide: const BorderSide(color: Color(0xFF1F293D)),
                              ),
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),

                  const SizedBox(height: 20),

                  // Engine Telemetry Card
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(
                            mainAxisAlignment: MainAxisAlignment.spaceBetween,
                            children: [
                              const Text('Personal SLM Stats', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 15)),
                              Text(
                                '${(_stats['memoryKb'] as num?)?.toStringAsFixed(1) ?? '0.0'} KB',
                                style: const TextStyle(color: Color(0xFF10B981), fontWeight: FontWeight.bold),
                              ),
                            ],
                          ),
                          const SizedBox(height: 14),
                          Row(
                            children: [
                              _buildMetricTile('Words', '${_stats['uniqueWords'] ?? 0}', 'Personal Vocab'),
                              _buildMetricTile('Tokens', '${_stats['totalTokens'] ?? 0}', 'Learned'),
                              _buildMetricTile('Bigrams', '${_stats['bigramEntries'] ?? 0}', '2-Words'),
                              _buildMetricTile('Trigrams', '${_stats['trigramEntries'] ?? 0}', '3-Words'),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ),

                  const SizedBox(height: 20),

                  // Optional Chat Import & Reset Actions
                  Card(
                    child: Padding(
                      padding: const EdgeInsets.all(16),
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          const Text('Data & Onboarding', style: TextStyle(fontWeight: FontWeight.bold, fontSize: 15)),
                          const SizedBox(height: 6),
                          const Text(
                            'Optionally bootstrap your personal vocabulary by importing a WhatsApp .txt chat export.',
                            style: TextStyle(fontSize: 12, color: Colors.white70),
                          ),
                          const SizedBox(height: 14),
                          Row(
                            children: [
                              Expanded(
                                child: OutlinedButton.icon(
                                  icon: const Icon(Icons.upload_file, size: 18),
                                  label: const Text('Import Chat (.txt)'),
                                  onPressed: _showImportDialog,
                                ),
                              ),
                              const SizedBox(width: 10),
                              OutlinedButton.icon(
                                icon: const Icon(Icons.delete_outline, color: Colors.redAccent, size: 18),
                                label: const Text('Reset', style: TextStyle(color: Colors.redAccent)),
                                onPressed: _clearModel,
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ),

                  const SizedBox(height: 20),

                  // Privacy Notice
                  const Center(
                    child: Padding(
                      padding: EdgeInsets.symmetric(horizontal: 16),
                      child: Text(
                        '🔒 Privacy First: 100% On-Device Learning.\nAutocomplete is off by default. Passwords, OTPs, and credit card numbers are strictly excluded from learning.',
                        textAlign: TextAlign.center,
                        style: TextStyle(fontSize: 11, color: Colors.white38, height: 1.5),
                      ),
                    ),
                  ),
                  const SizedBox(height: 20),
                ],
              ),
            ),
    );
  }

  Widget _buildMetricTile(String label, String value, String sub) {
    return Expanded(
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(fontSize: 11, color: Colors.white60, fontWeight: FontWeight.w600)),
          const SizedBox(height: 2),
          Text(value, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
          Text(sub, style: const TextStyle(fontSize: 10, color: Colors.white38)),
        ],
      ),
    );
  }
}
