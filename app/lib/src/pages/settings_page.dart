import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../ads.dart';
import '../drive_save_store.dart';
import '../settings.dart';
import '../theme.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.settings, required this.cloud});

  final AppSettings settings;
  final CloudAccount cloud;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late final Future<bool> _privacyOptionsRequired = privacyOptionsRequired();

  Future<void> _setShowAds(bool show) async {
    if (!show && !await _confirmTurnOffAds()) {
      return;
    }

    await widget.settings.setShowAds(show);
  }

  Future<bool> _confirmTurnOffAds() async {
    final turnOff = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Turn off ads?'),
        content: const Text('Ads help fund the development of Sogen. Do you really want to turn them off?'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Keep ads')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Turn off')),
        ],
      ),
    );

    return turnOff == true;
  }

  Future<void> _signIn() async {
    try {
      await widget.cloud.signIn();
    } on Exception catch (e) {
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Could not sign in: $e')));
      }
    }
  }

  Future<void> _signOut() async {
    final signOut = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Sign out of Google?'),
        content: const Text('Games stop syncing their saves. The saves stay in your Google Drive and on this device.'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Sign out')),
        ],
      ),
    );
    if (signOut == true) {
      await widget.cloud.signOut();
    }
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final subtitleStyle = theme.textTheme.bodySmall?.copyWith(color: SogenColors.neutral400);

    return Scaffold(
      appBar: AppBar(
        leading: IconButton(icon: const Icon(LucideIcons.x), onPressed: () => Navigator.of(context).pop()),
        title: Text('Settings', style: theme.textTheme.titleLarge?.copyWith(fontWeight: FontWeight.w700)),
      ),
      body: ListenableBuilder(
        listenable: Listenable.merge([widget.settings, widget.cloud]),
        builder: (context, _) => ListView(
          padding: const EdgeInsets.fromLTRB(24, 8, 24, 32),
          children: [
            if (CloudAccount.isAvailable)
              ListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Cloud saves'),
                subtitle: Text(
                  widget.cloud.isSignedIn
                      ? 'Signed in as ${widget.cloud.email}'
                      : 'Sign in with Google to keep saves in your Google Drive',
                  style: subtitleStyle,
                ),
                trailing: TextButton(
                  onPressed: widget.cloud.isSignedIn ? _signOut : _signIn,
                  child: Text(widget.cloud.isSignedIn ? 'Sign out' : 'Sign in'),
                ),
              ),
            SwitchListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Show ads'),
              subtitle: Text('When the app starts and after you leave a game', style: subtitleStyle),
              value: widget.settings.showAds,
              onChanged: _setShowAds,
            ),
            FutureBuilder(
              future: _privacyOptionsRequired,
              builder: (context, snapshot) => snapshot.data == true
                  ? ListTile(
                      contentPadding: EdgeInsets.zero,
                      title: const Text('Ad privacy options'),
                      subtitle: Text('Change your consent for personalised ads', style: subtitleStyle),
                      onTap: showPrivacyOptions,
                    )
                  : const SizedBox.shrink(),
            ),
          ],
        ),
      ),
    );
  }
}
