package dev.sogen.app

import android.net.Uri
import android.webkit.WebView
import org.json.JSONArray
import org.json.JSONException
import org.json.JSONObject

/**
 * Epic's login page, which ends on a redirect page that shows the authorization code as JSON instead of passing it
 * on in an address, so the code is read from the loaded page.
 */
class EpicSignInActivity : StoreSignInActivity() {
    override fun isLoginPage(url: Uri) = url.host == HOST && url.port == -1 && url.path == LOGIN_PATH

    override fun onPageLoaded(view: WebView, url: Uri) {
        if (!isRedirect(url)) {
            return
        }

        view.evaluateJavascript("document.body ? document.body.innerText : ''") { script ->
            complete(authorizationCode(script))
        }
    }

    companion object {
        private const val HOST = "www.epicgames.com"
        private const val LOGIN_PATH = "/id/login"
        private const val REDIRECT_PATH = "/id/api/redirect"

        private val epicOrigins = listOf(
            "https://epicgames.com",
            "https://www.epicgames.com",
            "https://store.epicgames.com",
            "https://login.epicgames.com",
        )

        fun isRedirect(url: Uri) = url.scheme == "https" && url.host == HOST && url.port == -1 && url.path == REDIRECT_PATH

        /** The script result is the page text as a JSON string; an empty code tells the Dart side the page was wrong. */
        fun authorizationCode(scriptResult: String?): String = try {
            val page = JSONObject(JSONArray("[${scriptResult.orEmpty()}]").getString(0))
            if (page.isNull("authorizationCode")) "" else page.getString("authorizationCode")
        } catch (e: JSONException) {
            ""
        }

        fun forgetLogin() = forgetLogin(epicOrigins, ".epicgames.com")
    }
}
