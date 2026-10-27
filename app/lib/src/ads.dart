import 'dart:async';

import 'package:google_mobile_ads/google_mobile_ads.dart';

import 'session.dart';
import 'settings.dart';

// Default to Google's sample units; release builds pass the real ones with --dart-define.
const _appOpenAdUnit = String.fromEnvironment(
  'ADMOB_APP_OPEN_AD_UNIT',
  defaultValue: 'ca-app-pub-3940256099942544/9257395921',
);
const _interstitialAdUnit = String.fromEnvironment(
  'ADMOB_INTERSTITIAL_AD_UNIT',
  defaultValue: 'ca-app-pub-3940256099942544/1033173712',
);

/// An app open ad while the app starts, and an interstitial when the user leaves a game.
class Ads {
  Ads(this._settings);

  static const _interstitialInterval = Duration(minutes: 5);
  static const _minimumSession = Duration(minutes: 1);

  final AppSettings _settings;
  DateTime? _lastShown;
  InterstitialAd? _interstitial;

  /// Loads the startup ad, after asking for consent where the user's region requires it.
  ///
  /// AdMob only allows this format on the loading screen, so if the ad is not ready within [timeout], not counting
  /// the time the user spends in the consent form, it is dropped instead of appearing over the library.
  Future<AppOpenAd?> loadStartupAd({Duration timeout = const Duration(seconds: 4)}) async {
    if (!_settings.showAds) {
      return null;
    }

    final waited = Stopwatch()..start();
    if (!await _gatherConsent(timeout, waited)) {
      return null;
    }

    final remaining = timeout - waited.elapsed;
    if (remaining <= Duration.zero) {
      return null;
    }

    final loaded = Completer<AppOpenAd?>();
    unawaited(_loadAppOpenAd(loaded));
    return loaded.future.timeout(
      remaining,
      onTimeout: () {
        unawaited(loaded.future.then((ad) => ad?.dispose()));
        return null;
      },
    );
  }

  Future<void> _loadAppOpenAd(Completer<AppOpenAd?> loaded) async {
    await MobileAds.instance.initialize();
    await AppOpenAd.load(
      adUnitId: _appOpenAdUnit,
      request: const AdRequest(),
      adLoadCallback: AppOpenAdLoadCallback(
        onAdLoaded: loaded.complete,
        onAdFailedToLoad: (_) => loaded.complete(null),
      ),
    );
  }

  void showStartupAd(AppOpenAd ad) {
    _lastShown = DateTime.now();
    ad
      ..fullScreenContentCallback = FullScreenContentCallback(
        onAdDismissedFullScreenContent: (ad) => ad.dispose(),
        onAdFailedToShowFullScreenContent: (ad, _) => ad.dispose(),
      )
      ..show();
  }

  /// Loads the interstitial while a game runs, so it is ready when the user leaves it.
  ///
  /// Consent is only asked for at startup; until it is given, no interstitial is loaded.
  Future<void> preloadInterstitial() async {
    if (!_settings.showAds || _interstitial != null || !await ConsentInformation.instance.canRequestAds()) {
      return;
    }

    await MobileAds.instance.initialize();
    await InterstitialAd.load(
      adUnitId: _interstitialAdUnit,
      request: const AdRequest(),
      adLoadCallback: InterstitialAdLoadCallback(onAdLoaded: (ad) => _interstitial = ad, onAdFailedToLoad: (_) {}),
    );
  }

  /// Shows the interstitial when the user stops or suspends a game they played for a while, at most once per
  /// [_interstitialInterval] counting the startup ad. Failed starts and crashes show none.
  void showAfterGame(GameSession session, {bool suspended = false}) {
    final ad = _interstitial;
    final startedAt = session.startedAt;
    final lastShown = _lastShown;
    final now = DateTime.now();
    final endedCleanly = suspended || (session.error == null && session.exitCode != null);

    if (ad == null ||
        !_settings.showAds ||
        !endedCleanly ||
        startedAt == null ||
        now.difference(startedAt) < _minimumSession ||
        (lastShown != null && now.difference(lastShown) < _interstitialInterval)) {
      return;
    }

    _interstitial = null;
    _lastShown = now;
    ad
      ..fullScreenContentCallback = FullScreenContentCallback(
        onAdDismissedFullScreenContent: (ad) => ad.dispose(),
        onAdFailedToShowFullScreenContent: (ad, _) => ad.dispose(),
      )
      ..show();
  }

  /// Without a timely consent update, falls back to the consent stored from earlier launches.
  Future<bool> _gatherConsent(Duration timeout, Stopwatch waited) async {
    final updated = Completer<bool>();
    ConsentInformation.instance.requestConsentInfoUpdate(
      ConsentRequestParameters(),
      () => updated.complete(true),
      (_) => updated.complete(false),
    );

    if (await updated.future.timeout(timeout, onTimeout: () => false)) {
      waited.stop();
      await ConsentForm.loadAndShowConsentFormIfRequired((_) {});
      waited.start();
    }

    return ConsentInformation.instance.canRequestAds();
  }
}

Future<bool> privacyOptionsRequired() async =>
    await ConsentInformation.instance.getPrivacyOptionsRequirementStatus() == PrivacyOptionsRequirementStatus.required;

Future<void> showPrivacyOptions() => ConsentForm.showPrivacyOptionsForm((_) {});
