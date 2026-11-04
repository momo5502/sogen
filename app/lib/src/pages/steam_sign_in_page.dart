import 'dart:async';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';
import 'package:qr_flutter/qr_flutter.dart';
import 'package:url_launcher/url_launcher.dart';

import '../steam/steam_auth.dart';
import '../steam/steam_http.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

/// Signs in to Steam by approving the sign-in in the Steam mobile app: by scanning the code with it, or by opening
/// the code's link on a device that has the app. Pops with the approved sign-in.
class SteamSignInPage extends StatefulWidget {
  const SteamSignInPage({super.key, required this.auth});

  final SteamAuth auth;

  @override
  State<SteamSignInPage> createState() => _SteamSignInPageState();
}

class _SteamSignInPageState extends State<SteamSignInPage> {
  SteamQrChallenge? _challenge;
  String? _error;
  Timer? _poll;
  bool _polling = false;

  @override
  void initState() {
    super.initState();
    _begin();
  }

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }

  Future<void> _begin() async {
    _poll?.cancel();
    setState(() {
      _challenge = null;
      _error = null;
    });
    try {
      final challenge = await widget.auth.beginQrSession('Sogen');
      if (!mounted) {
        return;
      }
      setState(() => _challenge = challenge);
      _poll = Timer.periodic(challenge.interval, (_) => _check());
    } on SteamException catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    }
  }

  Future<void> _check() async {
    final challenge = _challenge;
    if (challenge == null || _polling) {
      return;
    }
    _polling = true;
    try {
      final poll = await widget.auth.poll(challenge);
      if (!mounted) {
        return;
      }
      if (poll.approved) {
        _poll?.cancel();
        Navigator.of(context).pop(poll);
      } else if (poll.newChallengeUrl case final url?) {
        setState(() => _challenge = challenge.withChallengeUrl(url));
      }
    } on SteamAuthExpiredException {
      if (mounted) {
        unawaited(_begin());
      }
    } on SteamException catch (e) {
      if (mounted) {
        _poll?.cancel();
        setState(() => _error = e.message);
      }
    } finally {
      _polling = false;
    }
  }

  Future<void> _openInSteam(String url) async {
    final opened = await launchUrl(
      Uri.parse(url),
      mode: LaunchMode.externalApplication,
    ).catchError((Object _) => false);
    if (!opened && mounted) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('The Steam app is not installed.')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final challenge = _challenge;
    final error = _error;

    return Scaffold(
      appBar: AppBar(title: const Text('Sign in to Steam')),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420),
            child: error != null
                ? CenteredMessage(
                    icon: LucideIcons.cloudOff,
                    text: error,
                    action: PrimaryButton(label: 'Try again', icon: LucideIcons.refreshCw, onPressed: _begin),
                  )
                : Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Text(
                        'Scan this code with the Steam mobile app, or open it in the app on this device, and approve '
                        'the sign-in there. Sogen never sees your password.',
                        textAlign: TextAlign.center,
                        style: theme.textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
                      ),
                      const SizedBox(height: 24),
                      Container(
                        padding: const EdgeInsets.all(16),
                        decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(12)),
                        child: SizedBox.square(
                          dimension: 240,
                          child: challenge == null
                              ? const Center(child: CircularProgressIndicator(strokeWidth: 2))
                              : QrImageView(data: challenge.challengeUrl, padding: EdgeInsets.zero),
                        ),
                      ),
                      const SizedBox(height: 24),
                      PrimaryButton(
                        label: 'Open in the Steam app',
                        icon: LucideIcons.externalLink,
                        onPressed: challenge == null ? null : () => _openInSteam(challenge.challengeUrl),
                      ),
                    ],
                  ),
          ),
        ),
      ),
    );
  }
}
