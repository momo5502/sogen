import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class AppSettings extends ChangeNotifier {
  AppSettings._(this._preferences);

  static const _showAdsKey = 'showAds';

  final SharedPreferencesWithCache _preferences;

  static Future<AppSettings> load() async => AppSettings._(
    await SharedPreferencesWithCache.create(
      cacheOptions: const SharedPreferencesWithCacheOptions(allowList: {_showAdsKey}),
    ),
  );

  bool get showAds => _preferences.getBool(_showAdsKey) ?? true;

  Future<void> setShowAds(bool value) async {
    await _preferences.setBool(_showAdsKey, value);
    notifyListeners();
  }
}
