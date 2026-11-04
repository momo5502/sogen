import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:sogen_app/src/gog/gog_account.dart';
import 'package:sogen_app/src/gog/gog_http.dart';
import 'package:sogen_app/src/gog/gog_library.dart';

class _MemorySecrets implements SecretStorage {
  final values = <String, String>{};

  /// While set, writes wait for it, like a write the Keystore holds up.
  Completer<void>? writeBlocker;
  final writeStarted = Completer<void>();

  @override
  Future<String?> read(String name) async => values[name];

  @override
  Future<void> write(String name, String value) async {
    if (!writeStarted.isCompleted) {
      writeStarted.complete();
    }
    await writeBlocker?.future;
    values[name] = value;
  }

  @override
  Future<void> delete(String name) async => values.remove(name);
}

http.Response _json(Object body, {int status = 200}) => http.Response(jsonEncode(body), status);

Map<String, Object> _tokens(String refreshToken) => {
  'access_token': 'access-$refreshToken',
  'refresh_token': refreshToken,
  'expires_in': 3600,
};

Future<GogAccount> _signedIn(_MemorySecrets secrets, http.Client client, {GogLoginPage? loginPage}) async {
  secrets.values['gog'] = jsonEncode({'refreshToken': 'stored'});
  final account = GogAccount(secrets: secrets, loginPage: loginPage ?? (_) async => null, client: client);
  await account.restore();
  return account;
}

void main() {
  group('GogAccount', () {
    test('concurrent callers share one refresh and the replaced refresh token is stored', () async {
      final secrets = _MemorySecrets();
      final release = Completer<void>();
      var requests = 0;
      final account = await _signedIn(
        secrets,
        MockClient((request) async {
          requests++;
          expect(request.bodyFields['grant_type'], 'refresh_token');
          expect(request.bodyFields['refresh_token'], 'stored');
          await release.future;
          return _json(_tokens('rotated'));
        }),
      );

      final first = account.accessToken();
      final second = account.accessToken();
      release.complete();

      expect(await Future.wait([first, second]), ['access-rotated', 'access-rotated']);
      expect(requests, 1);
      expect(jsonDecode(secrets.values['gog']!), {'refreshToken': 'rotated'});
      expect(await account.accessToken(), 'access-rotated');
      expect(requests, 1);
    });

    test('a rejected grant signs out', () async {
      final secrets = _MemorySecrets();
      final account = await _signedIn(secrets, MockClient((_) async => _json({'error': 'invalid_grant'}, status: 400)));

      await expectLater(account.accessToken(), throwsA(isA<GogSignedOutException>()));
      await pumpEventQueue();
      expect(account.isSignedIn, isFalse);
      expect(secrets.values, isEmpty);
    });

    test('a server failure keeps the account signed in', () async {
      final secrets = _MemorySecrets();
      final account = await _signedIn(secrets, MockClient((_) async => _json({'error': 'server_error'}, status: 503)));

      await expectLater(
        account.accessToken(),
        throwsA(isA<GogException>().having((e) => e, 'signed out', isNot(isA<GogSignedOutException>()))),
      );
      expect(account.isSignedIn, isTrue);
      expect(secrets.values, contains('gog'));
    });

    test('a redirect without the state that was sent redeems no code', () async {
      final secrets = _MemorySecrets();
      var requests = 0;
      final account = GogAccount(
        secrets: secrets,
        loginPage: (url) async =>
            Uri.parse('https://embed.gog.com/on_login_success?origin=client&code=abc&state=forged'),
        client: MockClient((_) async {
          requests++;
          return _json(_tokens('new'));
        }),
      );

      await expectLater(account.signIn(), throwsA(isA<GogException>()));
      expect(requests, 0);
      expect(account.isSignedIn, isFalse);
    });

    test('signing in redeems the code GOG sent back with the state', () async {
      final secrets = _MemorySecrets();
      final account = GogAccount(
        secrets: secrets,
        loginPage: (url) async => Uri.parse(
          'https://embed.gog.com/on_login_success?origin=client&code=abc&state=${url.queryParameters['state']}',
        ),
        client: MockClient((request) async {
          expect(request.bodyFields['grant_type'], 'authorization_code');
          expect(request.bodyFields['code'], 'abc');
          return _json(_tokens('new'));
        }),
      );

      expect(await account.signIn(), isTrue);
      expect(account.isSignedIn, isTrue);
      expect(jsonDecode(secrets.values['gog']!), {'refreshToken': 'new'});
    });

    test('signing out while a refreshed token is being stored leaves the account signed out', () async {
      final secrets = _MemorySecrets()..writeBlocker = Completer<void>();
      final account = await _signedIn(secrets, MockClient((_) async => _json(_tokens('rotated'))));

      final refresh = account.accessToken();
      await secrets.writeStarted.future;
      final signOut = account.signOut();
      secrets.writeBlocker!.complete();

      await expectLater(refresh, throwsA(isA<GogSignedOutException>()));
      await signOut;
      expect(account.isSignedIn, isFalse);
      expect(secrets.values, isEmpty);
      await expectLater(account.accessToken(), throwsA(isA<GogSignedOutException>()));
    });
  });

  group('GogLibrary', () {
    Map<String, Object> product(int id, String title, {bool windows = true}) => {
      'id': id,
      'title': title,
      'image': '//images-1.gog-statics.com/$id',
      'worksOn': {'Windows': windows, 'Mac': !windows, 'Linux': false},
    };

    Future<GogLibrary> library(Map<String, Object> Function(int page) pages) async {
      final client = MockClient((request) async {
        if (request.url.host == 'auth.gog.com') {
          return _json(_tokens('rotated'));
        }

        expect(request.headers['Authorization'], 'Bearer access-rotated');
        return _json(pages(int.parse(request.url.queryParameters['page']!)));
      });
      return GogLibrary(await _signedIn(_MemorySecrets(), client), client: client);
    }

    test('collects the Windows games of every page once', () async {
      final gog = await library(
        (page) => {
          'totalPages': 2,
          'products': page == 1
              ? [product(2, 'beta'), product(3, 'Mac only', windows: false)]
              : [product(1, 'Alpha'), product(2, 'beta')],
        },
      );

      final games = await gog.games();
      expect(games.map((game) => game.productId), ['1', '2']);
      expect(games.map((game) => game.store).toSet(), {'gog'});
      expect(games.first.imageUrl, Uri.parse('https://images-1.gog-statics.com/1_196.jpg'));
    });

    test('a rejected access token is replaced once and the page requested again', () async {
      final secrets = _MemorySecrets();
      var refreshes = 0;
      var pageRequests = 0;
      final client = MockClient((request) async {
        if (request.url.host == 'auth.gog.com') {
          refreshes++;
          return _json(_tokens('rotated$refreshes'));
        }

        pageRequests++;
        if (request.headers['Authorization'] == 'Bearer access-rotated1') {
          return _json({'error': 'invalid_token'}, status: 401);
        }
        return _json({
          'totalPages': 1,
          'products': [product(1, 'Alpha')],
        });
      });
      final gog = GogLibrary(await _signedIn(secrets, client), client: client);

      expect((await gog.games()).single.productId, '1');
      expect(refreshes, 2);
      expect(pageRequests, 2);
    });

    test('a page rejected after a fresh token fails without retrying again', () async {
      var pageRequests = 0;
      final client = MockClient((request) async {
        if (request.url.host == 'auth.gog.com') {
          return _json(_tokens('rotated'));
        }

        pageRequests++;
        return _json({'error': 'invalid_token'}, status: 401);
      });
      final gog = GogLibrary(await _signedIn(_MemorySecrets(), client), client: client);

      await expectLater(gog.games(), throwsA(isA<GogException>()));
      expect(pageRequests, 2);
    });

    test('a product that cannot be read fails the library instead of hiding the game', () async {
      final gog = await library(
        (_) => {
          'totalPages': 1,
          'products': [
            product(1, 'Alpha'),
            {'id': 2, 'title': 'No platforms'},
          ],
        },
      );

      await expectLater(gog.games(), throwsA(isA<GogException>()));
    });

    test('a page count that changes between pages fails the library', () async {
      final gog = await library(
        (page) => {
          'totalPages': page == 1 ? 2 : 1,
          'products': [product(page, 'Game $page')],
        },
      );

      await expectLater(gog.games(), throwsA(isA<GogException>()));
    });

    test('a library beyond the page limit fails instead of showing part of it', () async {
      final gog = await library(
        (_) => {
          'totalPages': GogLibrary.maxPages + 1,
          'products': [product(1, 'Alpha')],
        },
      );

      await expectLater(gog.games(), throwsA(isA<GogException>()));
    });
  });
}
