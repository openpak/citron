// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import androidx.annotation.Keep
import androidx.core.content.edit
import androidx.preference.PreferenceManager
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.citron.citron_emu.CitronApplication

/**
 * OpenPak, as the app sees it: the same shared client and core the desktop build uses, reached
 * through one JSON bridge (openpak_native.cpp).
 *
 * Being online is not something anybody should have to ask for: [start] runs once the profile is
 * chosen, so the account is online, visible to friends and hearing about invitations before any
 * screen is opened. Everything that crosses into the client may wait on the network, so every call
 * here is off the main thread.
 */
object OpenPak {
    /** What the poll heard, for [OpenPakUi] to show. */
    sealed class Event {
        data class FriendOnline(val name: String, val game: String) : Event()
        data class FriendRequest(val name: String) : Event()
        data class Invitation(val name: String, val game: String) : Event()
        data class InvitationOffer(
            val id: String,
            val name: String,
            val game: String,
            val message: String,
            val createdAt: Long
        ) : Event()
        data class Message(val text: String) : Event()
        data class SavesPulled(val game: String) : Event()
        data class SavesPushed(val game: String) : Event()
        data class SavesPushFailed(val game: String, val error: String) : Event()
        data class SavesConflict(val game: String) : Event()
        data class PickFriends(val max: Int, val friends: List<PickableFriend>) : Event()
    }

    data class PickableFriend(val id: String, val name: String, val state: Int)

    data class Status(
        val enabled: Boolean,
        val redirect: Boolean,
        val signedIn: Boolean,
        val linked: Boolean,
        val websiteSignedIn: Boolean,
        val username: String,
        val nickname: String,
        val friendCode: String,
        val profileName: String,
        val server: String,
        val site: String,
        val running: String
    )

    data class Profile(
        val uuid: String,
        val name: String,
        val account: String,
        val current: Boolean
    )

    private external fun nativeInit()
    private external fun nativeStart(): Boolean
    private external fun nativeGoOffline()
    private external fun nativeCompatibility(programIdHex: String): String
    private external fun nativeCall(method: String, args: String): String

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val started = AtomicBoolean(false)
    private var pollJob: Job? = null

    private val _events = MutableSharedFlow<Event>(extraBufferCapacity = 64)
    val events: SharedFlow<Event> get() = _events

    /** Bumped when the site's compatibility list changed what this build shipped with. */
    private val _compatibilityVersion = MutableStateFlow(0)
    val compatibilityVersion: StateFlow<Int> get() = _compatibilityVersion

    private val preferences
        get() = PreferenceManager.getDefaultSharedPreferences(CitronApplication.appContext)

    /** Snackbars and notifications for friends, requests, invitations and saves (spec: Show notifications). */
    var notificationsEnabled: Boolean
        get() = preferences.getBoolean(PREF_NOTIFICATIONS, true)
        set(value) = preferences.edit { putBoolean(PREF_NOTIFICATIONS, value) }

    /** Cloud saves pulled before a game and pushed after it. */
    var cloudSyncEnabled: Boolean
        get() = preferences.getBoolean(PREF_CLOUD_SYNC, true)
        set(value) {
            preferences.edit { putBoolean(PREF_CLOUD_SYNC, value) }
            scope.launch { call("cloud_sync", JSONObject().put("enabled", value)) }
        }

    /** Which profile a plain launch uses: "" the last used, "ask", or a profile's UUID. */
    var startupProfile: String
        get() = preferences.getString(PREF_STARTUP_PROFILE, "") ?: ""
        set(value) = preferences.edit { putString(PREF_STARTUP_PROFILE, value) }

    /** The one-time setup (sign in, create an account, play offline) has been offered. */
    var setupOffered: Boolean
        get() = preferences.getBoolean(PREF_SETUP_OFFERED, false)
        set(value) = preferences.edit { putBoolean(PREF_SETUP_OFFERED, value) }

    /** Once, from Application.onCreate: no network, only who is asking and where files live. */
    fun init() {
        nativeInit()
        scope.launch {
            call(
                "set_device_name",
                JSONObject().put("name", "${CitronApplication.appContext.applicationInfo.loadLabel(CitronApplication.appContext.packageManager)} on ${android.os.Build.MODEL}")
            )
        }
        scope.launch { call("cloud_sync", JSONObject().put("enabled", cloudSyncEnabled)) }
    }

    /**
     * Online, and polling for what to tell the player. Idempotent: the game list and a game
     * started straight from a shortcut both call it.
     */
    fun start() {
        if (started.getAndSet(true)) {
            return
        }
        scope.launch {
            val signedIn = nativeStart()
            Log.info("[OpenPak] start(): ${if (signedIn) "signed in" else "not signed in"}")
            // What the site says right now about each title's online play, over the list this
            // build shipped with; the game list is drawn again only when that changes something.
            if (callObject("compatibility_refresh").optBoolean("changed")) {
                _compatibilityVersion.value++
            }
        }
        pollJob = scope.launch {
            while (isActive) {
                delay(POLL_MILLIS)
                val events = runCatching { callArray("poll") }.getOrNull() ?: continue
                for (index in 0 until events.length()) {
                    parseEvent(events.getJSONObject(index))?.let { _events.tryEmit(it) }
                }
            }
        }
    }

    /** Say goodbye, so friends see us leave now rather than when the presence lease runs out. */
    fun goOffline() {
        if (!started.getAndSet(false)) {
            return
        }
        pollJob?.cancel()
        pollJob = null
        Thread { nativeGoOffline() }.start()
    }

    private fun parseEvent(json: JSONObject): Event? =
        when (json.optString("type")) {
            "friend_online" -> Event.FriendOnline(json.optString("name"), json.optString("game"))
            "friend_request" -> Event.FriendRequest(json.optString("name"))
            "invitation" -> Event.Invitation(json.optString("name"), json.optString("game"))
            "invitation_offer" -> Event.InvitationOffer(
                json.optString("id"),
                json.optString("name"),
                json.optString("game"),
                json.optString("message"),
                json.optLong("created_at")
            )
            "message" -> Event.Message(json.optString("text"))
            "saves_pulled" -> Event.SavesPulled(json.optString("game"))
            "saves_pushed" -> Event.SavesPushed(json.optString("game"))
            "saves_push_failed" -> Event.SavesPushFailed(json.optString("game"), json.optString("error"))
            "saves_conflict" -> Event.SavesConflict(json.optString("game"))
            else -> null
        }

    /** The game list's OpenPak pill: "live", "beta", "alpha", or empty. Cheap; any thread. */
    fun compatibility(programIdHex: String): String =
        try {
            nativeCompatibility(programIdHex)
        } catch (e: UnsatisfiedLinkError) {
            ""
        }

    suspend fun call(method: String, args: JSONObject = JSONObject()): String =
        withContext(Dispatchers.IO) { nativeCall(method, args.toString()) }

    suspend fun callObject(method: String, args: JSONObject = JSONObject()): JSONObject =
        JSONObject(call(method, args))

    suspend fun callArray(method: String, args: JSONObject = JSONObject()): JSONArray =
        JSONArray(call(method, args))

    /** An action's failure, worded for people, or empty when it worked. */
    suspend fun action(method: String, args: JSONObject): String =
        callObject(method, args).optString("error")

    suspend fun status(): Status {
        val json = callObject("status")
        return Status(
            enabled = json.optBoolean("enabled"),
            redirect = json.optBoolean("redirect"),
            signedIn = json.optBoolean("signed_in"),
            linked = json.optBoolean("linked"),
            websiteSignedIn = json.optBoolean("website_signed_in"),
            username = json.optString("username"),
            nickname = json.optString("nickname"),
            friendCode = json.optString("friend_code"),
            profileName = json.optString("profile_name"),
            server = json.optString("server"),
            site = json.optString("site"),
            running = json.optString("running")
        )
    }

    suspend fun profiles(): List<Profile> {
        val array = callArray("profiles")
        return (0 until array.length()).map { index ->
            val item = array.getJSONObject(index)
            Profile(
                uuid = item.optString("uuid"),
                name = item.optString("name"),
                account = item.optString("account"),
                current = item.optBoolean("current")
            )
        }
    }

    /** Create ("create", with a name), "select", "rename", "remove" or set the "image" of a profile. */
    suspend fun profileAction(
        action: String,
        uuid: String = "",
        name: String = "",
        path: String = ""
    ): JSONObject =
        callObject(
            "profile_action",
            JSONObject().put("action", action).put("uuid", uuid).put("name", name).put("path", path)
        )

    suspend fun currentProfile(): Profile? = profiles().firstOrNull { it.current }

    /** Sign the current profile in. The JSON carries "error" (empty when it worked). */
    suspend fun signIn(email: String, password: String, adopt: Boolean): JSONObject =
        callObject(
            "sign_in",
            JSONObject().put("email", email).put("password", password).put("adopt", adopt)
        )

    suspend fun signOut() {
        call("sign_out")
    }

    /** The "Connect this emulator to OpenPak" switch: on goes online, off says goodbye. */
    suspend fun setEnabled(enabled: Boolean) {
        call("set_enabled", JSONObject().put("enabled", enabled))
        withContext(Dispatchers.IO) { NativeConfig.saveGlobalConfig() }
        if (enabled) start() else goOffline()
    }

    /** Anything that may have changed the current profile: the session follows it. */
    suspend fun profileChanged() {
        call("profile_changed")
    }

    /** A profile is being deleted: its account is revoked and forgotten with it. */
    suspend fun forgetProfile(uuid: String) {
        call("forget_profile", JSONObject().put("uuid", uuid))
    }

    /**
     * The MyPage applet's "invite friends", asked from the applet's worker thread, which waits
     * for [friendsPicked]. Nothing on screen to ask with is an empty answer at once.
     */
    @Keep
    @Suppress("unused")
    fun onPickFriends(max: Int, friendsJson: String) {
        val array = JSONArray(friendsJson)
        val friends = (0 until array.length()).map { index ->
            val item = array.getJSONObject(index)
            PickableFriend(item.optString("id"), item.optString("name"), item.optInt("state"))
        }
        if (OpenPakUi.currentActivity == null || !_events.tryEmit(Event.PickFriends(max, friends))) {
            friendsPicked(emptyList())
        }
    }

    fun friendsPicked(ids: List<String>) {
        scope.launch { call("friends_picked", JSONObject().put("ids", JSONArray(ids))) }
    }

    private const val POLL_MILLIS = 5000L
    private const val PREF_NOTIFICATIONS = "openpak_notifications"
    private const val PREF_CLOUD_SYNC = "openpak_cloud_sync"
    private const val PREF_STARTUP_PROFILE = "openpak_startup_profile"
    private const val PREF_SETUP_OFFERED = "openpak_setup_offered"
}
