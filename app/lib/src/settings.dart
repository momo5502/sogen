import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

class AppSettings extends ChangeNotifier {
  AppSettings._(this._preferences);

  static const _showAdsKey = 'showAds';
  static const _showSupportNoticeKey = 'showSupportNotice';

  final SharedPreferencesWithCache _preferences;

  static Future<AppSettings> load() async => AppSettings._(
    await SharedPreferencesWithCache.create(
      cacheOptions: const SharedPreferencesWithCacheOptions(allowList: {_showAdsKey, _showSupportNoticeKey}),
    ),
  );

  bool get showAds => _preferences.getBool(_showAdsKey) ?? true;

  Future<void> setShowAds(bool value) async {
    await _preferences.setBool(_showAdsKey, value);
    notifyListeners();
  }

  bool get showSupportNotice => _preferences.getBool(_showSupportNoticeKey) ?? true;

  Future<void> hideSupportNotice() async {
    await _preferences.setBool(_showSupportNoticeKey, false);
    notifyListeners();
  }
}
