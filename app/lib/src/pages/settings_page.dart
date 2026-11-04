import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';

import '../ads.dart';
import '../gog/gog_account.dart';
import '../settings.dart';
import '../support.dart';
import '../theme.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key, required this.settings, required this.gog, this.onRedownloadRoot});

  final AppSettings settings;
  final GogAccount gog;

  /// Downloads the emulation root again; null while it is not installed or an install is running.
  final VoidCallback? onRedownloadRoot;

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

  /// The download reports its progress on the home page, so Settings closes before it starts.
  void _redownloadRoot() {
    Navigator.of(context).pop();
    widget.onRedownloadRoot!();
  }

  Future<void> _signOutOfGog() async {
    final signOut = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('Sign out of GOG?'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Sign out')),
        ],
      ),
    );

    if (signOut == true) {
      await widget.gog.signOut();
    }
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
        listenable: Listenable.merge([widget.settings, widget.gog]),
        builder: (context, _) => ListView(
          padding: const EdgeInsets.fromLTRB(24, 8, 24, 32),
          children: [
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
            if (widget.onRedownloadRoot != null)
              ListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Re-download emulation root'),
                subtitle: Text('Replace the Windows files Sogen runs against', style: subtitleStyle),
                onTap: _redownloadRoot,
              ),
            ListTile(
              contentPadding: EdgeInsets.zero,
              title: const Text('Help and support'),
              subtitle: Text('Join the Sogen Discord', style: subtitleStyle),
              onTap: () => openDiscord(context),
            ),
            if (widget.gog.isSignedIn)
              ListTile(
                contentPadding: EdgeInsets.zero,
                title: const Text('Sign out of GOG'),
                subtitle: Text('Sogen forgets your GOG account', style: subtitleStyle),
                onTap: _signOutOfGog,
              ),
          ],
        ),
      ),
    );
  }
}
