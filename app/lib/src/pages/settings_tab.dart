import 'package:flutter/material.dart';

import '../ads.dart';
import '../settings.dart';
import '../store/store_account.dart';
import '../support.dart';
import '../theme.dart';

class SettingsTab extends StatefulWidget {
  const SettingsTab({super.key, required this.settings, required this.accounts, this.onRedownloadRoot});

  final AppSettings settings;
  final List<StoreSignIn> accounts;

  /// Downloads the emulation root again; null while it is not installed or an install is running.
  final VoidCallback? onRedownloadRoot;

  @override
  State<SettingsTab> createState() => _SettingsTabState();
}

class _SettingsTabState extends State<SettingsTab> {
  late final Future<bool> _privacyOptionsRequired = privacyOptionsRequired();

  Future<void> _setShowAds(bool show) async {
    if (!show && !await _confirmTurnOffAds()) {
      return;
    }

    await widget.settings.setShowAds(show);
  }

  Future<void> _signOut(StoreSignIn account) async {
    final signOut = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Sign out of ${account.service.name}?'),
        actions: [
          TextButton(onPressed: () => Navigator.of(context).pop(false), child: const Text('Cancel')),
          TextButton(onPressed: () => Navigator.of(context).pop(true), child: const Text('Sign out')),
        ],
      ),
    );

    if (signOut == true) {
      await account.signOut();
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

    return ListenableBuilder(
      listenable: Listenable.merge([widget.settings, ...widget.accounts]),
      builder: (context, _) => ListView(
        padding: const EdgeInsets.fromLTRB(24, 16, 24, 32),
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
              onTap: widget.onRedownloadRoot,
            ),
          ListTile(
            contentPadding: EdgeInsets.zero,
            title: const Text('Help and support'),
            subtitle: Text('Join the Sogen Discord', style: subtitleStyle),
            onTap: () => openDiscord(context),
          ),
          for (final account in widget.accounts)
            if (account.isSignedIn)
              ListTile(
                contentPadding: EdgeInsets.zero,
                title: Text('Sign out of ${account.service.name}'),
                subtitle: Text('Sogen forgets your ${account.service.name} account', style: subtitleStyle),
                onTap: () => _signOut(account),
              ),
        ],
      ),
    );
  }
}
