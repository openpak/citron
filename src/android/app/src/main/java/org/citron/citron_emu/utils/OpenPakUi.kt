// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.utils

import android.app.Activity
import android.app.Application
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.util.Base64
import android.view.View
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.button.MaterialButton
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.io.File
import java.lang.ref.WeakReference
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.citron.citron_emu.R
import org.citron.citron_emu.CitronApplication

/**
 * The OpenPak moments that are not a screen of their own, as the desktop host has them: which
 * profile plays at startup, the one-time setup (sign in, create an account, play offline), the
 * sign-in dialog, toasts, an invitation to the running game, and MyPage's friend picker.
 */
object OpenPakUi : Application.ActivityLifecycleCallbacks {
    private var current: WeakReference<FragmentActivity>? = null
    private var liveActivities = 0

    /** The activity on screen, for a dialog the network asked for. */
    val currentActivity: FragmentActivity?
        get() = current?.get()?.takeIf { !it.isFinishing && !it.isDestroyed }

    fun install(application: Application) {
        application.registerActivityLifecycleCallbacks(this)
        MainScope().launch { OpenPak.events.collect { show(it) } }
    }

    private fun context() = CitronApplication.appContext

    private fun toast(text: String) {
        Toast.makeText(context(), text, Toast.LENGTH_LONG).show()
    }

    private fun show(event: OpenPak.Event) {
        when (event) {
            is OpenPak.Event.Message -> toast(event.text)
            is OpenPak.Event.FriendOnline -> if (OpenPak.notificationsEnabled) {
                toast(
                    if (event.game.isEmpty()) {
                        context().getString(R.string.openpak_toast_online, event.name)
                    } else {
                        context().getString(R.string.openpak_toast_playing, event.name, event.game)
                    }
                )
            }
            is OpenPak.Event.FriendRequest -> if (OpenPak.notificationsEnabled) {
                toast(context().getString(R.string.openpak_toast_request, event.name))
            }
            is OpenPak.Event.Invitation -> if (OpenPak.notificationsEnabled) {
                toast(context().getString(R.string.openpak_toast_invitation, event.name, event.game))
            }
            is OpenPak.Event.InvitationOffer -> offerInvitation(event)
            is OpenPak.Event.PickFriends -> pickFriends(event)
        }
    }

    /** Join leaves the sender's data in the running game's invitation channel; either answer marks it read. */
    private fun offerInvitation(event: OpenPak.Event.InvitationOffer) {
        val activity = currentActivity ?: return // still listed under OpenPak, Invitations
        val answer = { join: Boolean ->
            activity.lifecycleScope.launch {
                val error = OpenPak.action(
                    "invitation_action",
                    org.json.JSONObject().put("id", event.id).put("source", "console")
                        .put("action", if (join) "join" else "dismiss")
                )
                if (error.isNotEmpty()) toast(error)
            }
        }
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_invitation_title)
            .setMessage(activity.getString(R.string.openpak_invitation_message, event.name, event.game))
            .setPositiveButton(R.string.openpak_join) { _, _ -> answer(true) }
            .setNegativeButton(R.string.openpak_ignore) { _, _ -> answer(false) }
            .setCancelable(false)
            .show()
    }

    /** MyPage's "invite friends": up to the game's number of friends, online ones first. */
    private fun pickFriends(event: OpenPak.Event.PickFriends) {
        val activity = currentActivity
        if (activity == null || event.friends.isEmpty()) {
            if (activity != null) {
                MaterialAlertDialogBuilder(activity)
                    .setTitle(R.string.openpak_invite_friends)
                    .setMessage(R.string.openpak_no_friends)
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
            }
            OpenPak.friendsPicked(emptyList())
            return
        }
        val labels = event.friends.map {
            val state = when (it.state) {
                2 -> activity.getString(R.string.openpak_state_playing)
                1 -> activity.getString(R.string.openpak_state_online)
                else -> activity.getString(R.string.openpak_state_offline)
            }
            "${it.name} ($state)"
        }.toTypedArray()
        val checked = BooleanArray(labels.size)
        var answered = false
        val dialog = MaterialAlertDialogBuilder(activity)
            .setTitle(
                if (event.max == 1) {
                    activity.getString(R.string.openpak_invite_one)
                } else {
                    activity.getString(R.string.openpak_invite_many, event.max)
                }
            )
            .setMultiChoiceItems(labels, checked) { dialog, which, isChecked ->
                // More than the game allows is refused here, not silently trimmed later.
                if (isChecked && checked.count { it } > event.max) {
                    checked[which] = false
                    (dialog as AlertDialog).listView.setItemChecked(which, false)
                }
            }
            .setPositiveButton(R.string.openpak_invite) { _, _ ->
                answered = true
                OpenPak.friendsPicked(event.friends.filterIndexed { i, _ -> checked[i] }.map { it.id })
            }
            .setNegativeButton(android.R.string.cancel, null)
            .setOnDismissListener {
                if (!answered) {
                    answered = true
                    OpenPak.friendsPicked(emptyList())
                }
            }
            .show()
        dialog.setCanceledOnTouchOutside(false)
    }

    // ---- startup: which profile, set it up once, then online ----

    /** A plain launch of the game list: the desktop host's RunStartup. */
    fun runStartup(activity: FragmentActivity) {
        activity.lifecycleScope.launch {
            val status = OpenPak.status()
            if (!status.redirect) {
                return@launch
            }
            pickStartupProfile(activity) {
                OpenPak.start()
                activity.lifecycleScope.launch {
                    // Set up once, ever: sign in, create an account, or play offline -- after
                    // which the profile is the person's own either way.
                    if (!OpenPak.setupOffered && !OpenPak.status().websiteSignedIn) {
                        OpenPak.setupOffered = true
                        runSetup(activity, false) {}
                    }
                }
            }
        }
    }

    private fun pickStartupProfile(activity: FragmentActivity, then: () -> Unit) {
        activity.lifecycleScope.launch {
            val profiles = OpenPak.profiles()
            val choice = OpenPak.startupProfile
            if (profiles.size < 2 || choice.isEmpty()) {
                then() // one profile, or the last used, which the setting already is
                return@launch
            }
            if (choice != STARTUP_ASK) {
                profiles.firstOrNull { it.uuid == choice && !it.current }?.let { selectProfile(it.uuid) }
                then()
                return@launch
            }
            showProfilePicker(activity, profiles, then)
        }
    }

    /** "Who is playing?": the profiles with their OpenPak or offline badge, and Add account. */
    fun showProfilePicker(
        activity: FragmentActivity,
        profiles: List<OpenPak.Profile>,
        then: () -> Unit
    ) {
        val labels = profiles.map {
            "${it.name}\n" + if (it.account.isEmpty()) {
                activity.getString(R.string.openpak_offline)
            } else {
                activity.getString(R.string.openpak_badge, it.account)
            }
        }.toTypedArray()
        var selected = profiles.indexOfFirst { it.current }.coerceAtLeast(0)
        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_who_is_playing)
            .setSingleChoiceItems(labels, selected) { _, which -> selected = which }
            .setPositiveButton(R.string.openpak_continue) { _, _ ->
                activity.lifecycleScope.launch {
                    if (!profiles[selected].current) selectProfile(profiles[selected].uuid)
                    then()
                }
            }
            .setNeutralButton(R.string.openpak_add_account) { _, _ -> runSetup(activity, true, then) }
            .setNegativeButton(android.R.string.cancel) { _, _ -> then() } // keeps the last used
            .setCancelable(false)
            .show()
    }

    /** Make a profile the current one; the session takes the old account offline and signs this in. */
    suspend fun selectProfile(uuid: String) {
        OpenPak.profileAction("select", uuid)
        withContext(Dispatchers.IO) { NativeConfig.saveGlobalConfig() }
    }

    /**
     * The setup: Sign in with OpenPak, Create an OpenPak account (the website's register page,
     * then sign in once the email is verified), or Play offline with a name. [addAccount] makes a
     * new profile for it; otherwise the current profile is set up.
     */
    fun runSetup(activity: FragmentActivity, addAccount: Boolean, done: () -> Unit) {
        val padding = (24 * activity.resources.displayMetrics.density).toInt()
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding / 2, padding, 0)
        }
        layout.addView(TextView(activity).apply { setText(R.string.openpak_setup_message) })
        val signIn = MaterialButton(activity).apply { setText(R.string.openpak_sign_in_with) }
        val create = MaterialButton(activity).apply { setText(R.string.openpak_create_account) }
        val offline = MaterialButton(activity).apply { setText(R.string.openpak_play_offline) }
        listOf(signIn, create, offline).forEach { layout.addView(it) }

        val builder = MaterialAlertDialogBuilder(activity)
            .setTitle(if (addAccount) R.string.openpak_add_account else R.string.openpak_setup_title)
            .setView(layout)
            .setCancelable(false)
        if (addAccount) {
            builder.setNegativeButton(android.R.string.cancel) { _, _ -> done() }
        }
        val dialog = builder.show()

        signIn.setOnClickListener {
            dialog.dismiss()
            setupSignIn(activity, addAccount, null, done)
        }
        create.setOnClickListener {
            dialog.dismiss()
            activity.lifecycleScope.launch {
                val site = OpenPak.status().site
                runCatching {
                    activity.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse("$site/register")))
                }
                setupSignIn(
                    activity,
                    addAccount,
                    activity.getString(R.string.openpak_verify_then_sign_in),
                    done
                )
            }
        }
        offline.setOnClickListener {
            dialog.dismiss()
            setupOffline(activity, addAccount, done)
        }
    }

    private fun setupOffline(activity: FragmentActivity, addAccount: Boolean, done: () -> Unit) {
        activity.lifecycleScope.launch {
            val suggested = if (addAccount) "" else OpenPak.status().profileName
            val name = EditText(activity).apply {
                setText(suggested)
                inputType = InputType.TYPE_CLASS_TEXT
            }
            MaterialAlertDialogBuilder(activity)
                .setTitle(R.string.openpak_profile_name)
                .setView(frame(activity, name))
                .setPositiveButton(android.R.string.ok) { _, _ ->
                    val chosen = name.text.toString().trim().take(32)
                    if (chosen.isEmpty()) {
                        runSetup(activity, addAccount, done)
                        return@setPositiveButton
                    }
                    activity.lifecycleScope.launch {
                        if (addAccount) {
                            val created = OpenPak.profileAction("create", name = chosen)
                            if (created.optString("error").isEmpty()) {
                                selectProfile(created.optString("uuid"))
                            } else {
                                toast(created.optString("error"))
                            }
                        } else {
                            OpenPak.currentProfile()?.let { OpenPak.profileAction("rename", it.uuid, chosen) }
                        }
                        done()
                    }
                }
                .setNegativeButton(android.R.string.cancel) { _, _ -> runSetup(activity, addAccount, done) }
                .setCancelable(false)
                .show()
        }
    }

    /** The account is kept under the current profile, so a new one is made current first and taken away again if nobody signs in. */
    private fun setupSignIn(
        activity: FragmentActivity,
        addAccount: Boolean,
        intro: String?,
        done: () -> Unit
    ) {
        activity.lifecycleScope.launch {
            val previous = OpenPak.currentProfile()?.uuid
            var created: String? = null
            if (addAccount) {
                val made = OpenPak.profileAction("create", name = "OpenPak")
                if (made.optString("error").isNotEmpty()) {
                    toast(made.optString("error"))
                    done()
                    return@launch
                }
                created = made.optString("uuid")
                selectProfile(made.optString("uuid"))
            }
            showSignIn(activity, adopt = true, intro = intro) { signedIn ->
                if (signedIn) {
                    done()
                    return@showSignIn
                }
                activity.lifecycleScope.launch {
                    created?.let {
                        if (!previous.isNullOrEmpty()) selectProfile(previous)
                        OpenPak.profileAction("remove", it)
                    }
                    runSetup(activity, addAccount, done)
                }
            }
        }
    }

    /**
     * Email and password, sent to OpenPak over TLS and nowhere else. [adopt] copies the account's
     * name and picture into the profile, which only the setup does. A refusal comes back to this
     * dialog with the reason on it.
     */
    fun showSignIn(
        activity: FragmentActivity,
        adopt: Boolean,
        intro: String? = null,
        error: String? = null,
        lastEmail: String = "",
        done: (Boolean) -> Unit = {}
    ) {
        val padding = (24 * activity.resources.displayMetrics.density).toInt()
        val layout = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(padding, padding / 2, padding, 0)
        }
        listOfNotNull(intro, error).forEach { text ->
            layout.addView(TextView(activity).apply { this.text = text })
        }
        val email = EditText(activity).apply {
            hint = activity.getString(R.string.openpak_email)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS
            setText(lastEmail)
        }
        val password = EditText(activity).apply {
            hint = activity.getString(R.string.openpak_password)
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
        }
        layout.addView(email)
        layout.addView(password)

        MaterialAlertDialogBuilder(activity)
            .setTitle(R.string.openpak_sign_in)
            .setView(layout)
            .setPositiveButton(R.string.openpak_sign_in) { _, _ ->
                val address = email.text.toString().trim()
                activity.lifecycleScope.launch {
                    toast(activity.getString(R.string.openpak_signing_in))
                    val result = OpenPak.signIn(address, password.text.toString(), adopt)
                    val failure = result.optString("error")
                    if (failure.isNotEmpty()) {
                        showSignIn(activity, adopt, intro, failure, address, done)
                        return@launch
                    }
                    val username = result.optString("username")
                    val linkError = result.optString("link_error")
                    toast(
                        if (linkError.isEmpty()) {
                            activity.getString(R.string.openpak_signed_in_linked, username)
                        } else {
                            activity.getString(R.string.openpak_signed_in_unlinked, username, linkError)
                        }
                    )
                    if (adopt) adoptAccount(activity, username, result.optString("avatar"))
                    done(true)
                }
            }
            .setNegativeButton(android.R.string.cancel) { _, _ -> done(false) }
            .setCancelable(false)
            .show()
    }

    /** The account's name and picture, copied into the current profile once. */
    private suspend fun adoptAccount(activity: Activity, username: String, avatar: String) {
        val uuid = OpenPak.currentProfile()?.uuid ?: return
        if (username.isNotEmpty()) {
            OpenPak.profileAction("rename", uuid, username.take(32))
        }
        withContext(Dispatchers.IO) {
            if (avatar.isEmpty()) return@withContext
            runCatching {
                val bytes = Base64.decode(avatar, Base64.DEFAULT)
                val source = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return@runCatching
                val side = minOf(source.width, source.height)
                val square = Bitmap.createBitmap(
                    source,
                    (source.width - side) / 2,
                    (source.height - side) / 2,
                    side,
                    side
                )
                val scaled = Bitmap.createScaledBitmap(square, 256, 256, true)
                val file = File(activity.cacheDir, "openpak-avatar.jpg")
                file.outputStream().use { scaled.compress(Bitmap.CompressFormat.JPEG, 90, it) }
                kotlinx.coroutines.runBlocking {
                    OpenPak.profileAction("image", uuid, path = file.absolutePath)
                }
                file.delete()
            }
        }
    }

    fun frame(activity: Activity, view: View): View {
        val padding = (24 * activity.resources.displayMetrics.density).toInt()
        return LinearLayout(activity).apply {
            setPadding(padding, padding / 2, padding, 0)
            addView(
                view,
                LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT,
                    LinearLayout.LayoutParams.WRAP_CONTENT
                )
            )
        }
    }

    // ---- which activity is on screen, and the goodbye when the last one goes ----

    override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) {
        liveActivities++
    }

    override fun onActivityResumed(activity: Activity) {
        if (activity is FragmentActivity) current = WeakReference(activity)
    }

    override fun onActivityPaused(activity: Activity) {
        if (current?.get() === activity) current = null
    }

    override fun onActivityDestroyed(activity: Activity) {
        liveActivities--
        // The app is closing, not rotating: friends should see us leave now.
        if (liveActivities <= 0 && activity.isFinishing && !activity.isChangingConfigurations) {
            OpenPak.goOffline()
        }
    }

    override fun onActivityStarted(activity: Activity) {}
    override fun onActivityStopped(activity: Activity) {}
    override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) {}

    const val STARTUP_ASK = "ask"
}
