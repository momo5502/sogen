import 'dart:async';

import 'package:flutter/material.dart';
import 'package:lucide_icons_flutter/lucide_icons.dart';
import 'package:qr_flutter/qr_flutter.dart';

import '../steam/steam_auth.dart';
import '../steam/steam_http.dart';
import '../theme.dart';
import '../widgets/sogen_widgets.dart';

/// Signs in to Steam, either by approving a QR code in the Steam mobile app on another device, or with the account's
/// password and a Steam Guard confirmation. Pops with the approved sign-in.
class SteamSignInPage extends StatefulWidget {
  const SteamSignInPage({super.key, required this.auth});

  final SteamAuth auth;

  @override
  State<SteamSignInPage> createState() => _SteamSignInPageState();
}

enum _Method { qr, password }

class _SteamSignInPageState extends State<SteamSignInPage> {
  _Method _method = _Method.qr;
  SteamAuthSession? _session;
  String? _error;
  Timer? _poll;
  bool _polling = false;
  bool _working = false;

  /// Set once the QR code was scanned; the code then stays as it is so a rotation cannot race the approval.
  bool _scanned = false;

  final _account = TextEditingController();
  final _password = TextEditingController();
  final _code = TextEditingController();

  @override
  void initState() {
    super.initState();
    _beginQr();
  }

  @override
  void dispose() {
    _poll?.cancel();
    _account.dispose();
    _password.dispose();
    _code.dispose();
    super.dispose();
  }

  void _switch(_Method method) {
    if (method == _method) {
      return;
    }
    _poll?.cancel();
    setState(() {
      _method = method;
      _session = null;
      _error = null;
      _scanned = false;
    });
    if (method == _Method.qr) {
      _beginQr();
    }
  }

  Future<void> _beginQr() async {
    _poll?.cancel();
    setState(() {
      _session = null;
      _error = null;
      _scanned = false;
    });
    try {
      final challenge = await widget.auth.beginQrSession('Sogen');
      if (mounted && _method == _Method.qr) {
        _start(challenge);
      }
    } on SteamException catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    }
  }

  /// The password only lives in its field until it is sent to Steam.
  Future<void> _beginPassword() async {
    final account = _account.text.trim();
    final password = _password.text;
    if (account.isEmpty || password.isEmpty) {
      return;
    }
    setState(() {
      _working = true;
      _error = null;
    });
    try {
      final session = await widget.auth.beginCredentialsSession('Sogen', account, password);
      _password.clear();
      if (mounted) {
        _start(session);
      }
    } on SteamException catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    } finally {
      if (mounted) {
        setState(() => _working = false);
      }
    }
  }

  Future<void> _submitCode(SteamCredentialsSession session) async {
    final code = _code.text.trim();
    if (code.isEmpty) {
      return;
    }
    final guard = session.guards.contains(SteamGuard.deviceCode) ? SteamGuard.deviceCode : SteamGuard.emailCode;
    setState(() {
      _working = true;
      _error = null;
    });
    try {
      await widget.auth.submitGuardCode(session, guard, code);
      unawaited(_check());
    } on SteamWrongCodeException catch (e) {
      if (mounted) {
        setState(() => _error = e.message);
      }
    } on SteamException catch (e) {
      if (mounted) {
        _poll?.cancel();
        setState(() {
          _session = null;
          _error = e.message;
        });
      }
    } finally {
      if (mounted) {
        setState(() => _working = false);
      }
    }
  }

  void _start(SteamAuthSession session) {
    _poll?.cancel();
    setState(() => _session = session);
    _poll = Timer.periodic(session.interval, (_) => _check());
    if (session is SteamCredentialsSession && session.guards.isEmpty) {
      unawaited(_check());
    }
  }

  Future<void> _check() async {
    final session = _session;
    if (session == null || _polling) {
      return;
    }
    _polling = true;
    try {
      final poll = await widget.auth.poll(session);
      if (!mounted || !identical(session, _session)) {
        return;
      }
      if (poll.approved) {
        _poll?.cancel();
        Navigator.of(context).pop(poll);
      } else if (session is SteamQrChallenge) {
        setState(() {
          _scanned = _scanned || poll.scanned;
          _session = session.rotated(clientId: poll.newClientId, challengeUrl: poll.newChallengeUrl);
        });
      }
    } on SteamAuthExpiredException catch (e) {
      if (mounted) {
        if (_method == _Method.qr) {
          unawaited(_beginQr());
        } else {
          _poll?.cancel();
          setState(() {
            _session = null;
            _error = e.message;
          });
        }
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

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Sign in to Steam')),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 420),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                SegmentedButton<_Method>(
                  segments: const [
                    ButtonSegment(value: _Method.qr, icon: Icon(LucideIcons.qrCode), label: Text('QR code')),
                    ButtonSegment(value: _Method.password, icon: Icon(LucideIcons.keyRound), label: Text('Password')),
                  ],
                  selected: {_method},
                  onSelectionChanged: (selection) => _switch(selection.single),
                ),
                const SizedBox(height: 24),
                switch (_method) {
                  _Method.qr => _qr(context),
                  _Method.password => _passwordForm(context),
                },
              ],
            ),
          ),
        ),
      ),
    );
  }

  Widget _text(BuildContext context, String text) => Text(
    text,
    textAlign: TextAlign.center,
    style: Theme.of(context).textTheme.bodyMedium?.copyWith(color: SogenColors.neutral300, height: 1.5),
  );

  Widget _errorLine(BuildContext context) => Padding(
    padding: const EdgeInsets.only(top: 16),
    child: Text(
      _error!,
      textAlign: TextAlign.center,
      style: Theme.of(context).textTheme.bodyMedium?.copyWith(color: Theme.of(context).colorScheme.error),
    ),
  );

  Widget _qr(BuildContext context) {
    final error = _error;
    if (error != null) {
      return CenteredMessage(
        icon: LucideIcons.cloudOff,
        text: error,
        action: PrimaryButton(label: 'Try again', icon: LucideIcons.refreshCw, onPressed: _beginQr),
      );
    }
    if (_scanned) {
      return const CenteredMessage(
        icon: LucideIcons.smartphone,
        text: 'Approve the sign-in in the Steam app. Sogen continues once you did.',
      );
    }

    final challenge = _session;
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        _text(
          context,
          'Scan this code with the Steam mobile app on another device and approve the sign-in there. Sogen never '
          'sees your password.',
        ),
        const SizedBox(height: 24),
        Container(
          padding: const EdgeInsets.all(16),
          decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(12)),
          child: SizedBox.square(
            dimension: 240,
            child: challenge is SteamQrChallenge
                ? QrImageView(data: challenge.challengeUrl, padding: EdgeInsets.zero)
                : const Center(child: CircularProgressIndicator(strokeWidth: 2)),
          ),
        ),
      ],
    );
  }

  Widget _passwordForm(BuildContext context) {
    final session = _session;
    if (session is SteamCredentialsSession) {
      return _guard(context, session);
    }

    return AutofillGroup(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          _text(
            context,
            'Your password goes to Steam encrypted with Steam\'s key. Sogen does not keep it; it only keeps the sign-in '
            'Steam hands out.',
          ),
          const SizedBox(height: 24),
          TextField(
            controller: _account,
            enabled: !_working,
            autofillHints: const [AutofillHints.username],
            autocorrect: false,
            decoration: const InputDecoration(labelText: 'Steam account name'),
          ),
          const SizedBox(height: 12),
          TextField(
            controller: _password,
            enabled: !_working,
            obscureText: true,
            autofillHints: const [AutofillHints.password],
            decoration: const InputDecoration(labelText: 'Password'),
            onSubmitted: (_) => _beginPassword(),
          ),
          const SizedBox(height: 20),
          PrimaryButton(label: 'Sign in', icon: LucideIcons.logIn, onPressed: _working ? null : _beginPassword),
          if (_error != null) _errorLine(context),
        ],
      ),
    );
  }

  /// What Steam Guard asks for: a code, an approval in the app, or both, of which either confirms the sign-in.
  Widget _guard(BuildContext context, SteamCredentialsSession session) {
    final approve = session.guards.contains(SteamGuard.deviceConfirmation);
    final emailLink = session.guards.contains(SteamGuard.emailConfirmation);
    final code = session.guards.contains(SteamGuard.deviceCode)
        ? 'the code from the Steam Guard authenticator'
        : session.guards.contains(SteamGuard.emailCode)
        ? 'the code Steam sent you by email'
        : null;

    final instructions = [
      if (approve) 'Approve the sign-in in the Steam app',
      if (emailLink) 'Open the link Steam sent you by email',
      if (code != null) 'enter $code',
    ];
    final text = instructions.isEmpty
        ? 'Signing in…'
        : '${instructions.join(', or ')}.'.replaceFirstMapped(RegExp('^.'), (m) => m[0]!.toUpperCase());

    return Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _text(context, text),
        if (code != null) ...[
          const SizedBox(height: 24),
          TextField(
            controller: _code,
            enabled: !_working,
            autocorrect: false,
            textCapitalization: TextCapitalization.characters,
            autofillHints: const [AutofillHints.oneTimeCode],
            decoration: const InputDecoration(labelText: 'Steam Guard code'),
            onSubmitted: (_) => _submitCode(session),
          ),
          const SizedBox(height: 20),
          PrimaryButton(
            label: 'Confirm',
            icon: LucideIcons.check,
            onPressed: _working ? null : () => _submitCode(session),
          ),
        ] else
          const Padding(
            padding: EdgeInsets.only(top: 24),
            child: Center(child: SizedBox.square(dimension: 24, child: CircularProgressIndicator(strokeWidth: 2))),
          ),
        if (_error != null) _errorLine(context),
      ],
    );
  }
}
