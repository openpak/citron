// SPDX-FileCopyrightText: Copyright 2026 OpenPak
// SPDX-License-Identifier: GPL-3.0-or-later

package org.citron.citron_emu.fragments

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.BitmapFactory
import android.graphics.Typeface
import android.net.Uri
import android.os.Bundle
import android.text.InputType
import android.text.format.Formatter
import android.util.Base64
import android.view.Gravity
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.updatePadding
import androidx.fragment.app.DialogFragment
import androidx.fragment.app.activityViewModels
import androidx.lifecycle.lifecycleScope
import com.google.android.material.appbar.MaterialToolbar
import com.google.android.material.button.MaterialButton
import com.google.android.material.card.MaterialCardView
import com.google.android.material.color.MaterialColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.imageview.ShapeableImageView
import com.google.android.material.materialswitch.MaterialSwitch
import com.google.android.material.progressindicator.LinearProgressIndicator
import com.google.android.material.shape.ShapeAppearanceModel
import java.io.ByteArrayOutputStream
import java.io.File
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import org.citron.citron_emu.R
import org.citron.citron_emu.model.Game
import org.citron.citron_emu.model.GamesViewModel
import org.citron.citron_emu.utils.OpenPak
import org.citron.citron_emu.utils.OpenPakUi

/**
 * The OpenPak screen (openpak-ux-spec.md 4.2): a home with the identity card and the seven
 * sections in the window's order -- Account, Friends, Invitations, Cloud saves, Mods, News, Status
 * -- one screen each with the desktop pages' content and words, and the OpenPak settings (3.13).
 * The same file, package names aside, in every Android emulator.
 */
class OpenPakFragment : DialogFragment() {
    private val gamesViewModel: GamesViewModel by activityViewModels()

    private lateinit var toolbar: MaterialToolbar
    private lateinit var busy: LinearProgressIndicator
    private lateinit var content: LinearLayout
    private lateinit var statusLine: TextView
    private var page = HOME
    private var loads = 0

    /** The title the Mods and News screens look at: 16 hex digits and a name. */
    private var chosenTitle: Pair<String, String>? = null

    private val pickPicture =
        registerForActivityResult(ActivityResultContracts.GetContent()) { uri -> uri?.let { changePicture(it) } }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setStyle(STYLE_NO_TITLE, 0)
        page = savedInstanceState?.getInt(KEY_PAGE) ?: arguments?.getInt(KEY_PAGE, HOME) ?: HOME
    }

    override fun onStart() {
        super.onStart()
        dialog?.window?.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(KEY_PAGE, page)
    }

    override fun onCreateView(inflater: LayoutInflater, container: ViewGroup?, savedInstanceState: Bundle?): View {
        val context = requireActivity()
        val root = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(MaterialColors.getColor(context, com.google.android.material.R.attr.colorSurface, 0))
        }
        toolbar = MaterialToolbar(context).apply {
            setNavigationIcon(R.drawable.ic_back)
            setNavigationOnClickListener { if (page == HOME) dismiss() else show(HOME) }
            menu.add(R.string.openpak_common_refresh).setIcon(R.drawable.ic_refresh)
                .setShowAsAction(android.view.MenuItem.SHOW_AS_ACTION_ALWAYS)
            setOnMenuItemClickListener {
                show(page)
                true
            }
        }
        busy = LinearProgressIndicator(context).apply {
            isIndeterminate = true
            visibility = View.INVISIBLE
        }
        content = LinearLayout(context).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(8), dp(16), dp(16))
        }
        statusLine = TextView(context).apply {
            setPadding(dp(16), dp(8), dp(16), dp(8))
            alpha = 0.8f
            visibility = View.GONE
        }
        root.addView(toolbar)
        root.addView(busy)
        root.addView(ScrollView(context).apply { addView(content) }, LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f))
        root.addView(statusLine)
        ViewCompat.setOnApplyWindowInsetsListener(root) { view, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            view.updatePadding(left = bars.left, top = bars.top, right = bars.right, bottom = bars.bottom)
            insets
        }
        return root
    }

    override fun onViewCreated(view: View, savedInstanceState: Bundle?) {
        super.onViewCreated(view, savedInstanceState)
        dialog?.setOnKeyListener { _, keyCode, event ->
            if (keyCode == android.view.KeyEvent.KEYCODE_BACK && event.action == android.view.KeyEvent.ACTION_UP && page != HOME) {
                show(HOME)
                true
            } else {
                false
            }
        }
        show(page)
        // Friends and Invitations every 30 s while they show, Status too (spec 3.6).
        viewLifecycleOwner.lifecycleScope.launch {
            while (isActive) {
                delay(30_000)
                if (page == FRIENDS || page == INVITATIONS || page == STATUS) show(page, quiet = true)
            }
        }
    }

    private fun show(index: Int, quiet: Boolean = false) {
        page = index
        toolbar.title = getString(TITLES[index] ?: R.string.openpak_menu_title)
        if (!quiet) statusLine.visibility = View.GONE
        loads++
        busy.visibility = View.VISIBLE
        viewLifecycleOwner.lifecycleScope.launch {
            val failure = runCatching {
                when (index) {
                    HOME -> showHome()
                    ACCOUNT -> showAccount()
                    FRIENDS -> showFriends()
                    INVITATIONS -> showInvitations()
                    SAVES -> showCloudSaves()
                    MODS -> showMods()
                    NEWS -> showNews()
                    STATUS -> showStatus()
                    else -> showSettings()
                }
            }.exceptionOrNull()
            if (--loads == 0) busy.visibility = View.INVISIBLE
            // A failed refresh keeps what is shown and says so in the status line.
            if (failure != null && isAdded) say(failure.message ?: failure.toString())
        }
    }

    /** The status line: the latest result or error of anything done on this screen. */
    private fun say(text: String) {
        statusLine.text = text
        statusLine.visibility = if (text.isEmpty()) View.GONE else View.VISIBLE
    }

    private fun begin(index: Int): Boolean {
        if (page != index || !isAdded) return false
        content.removeAllViews()
        return true
    }

    private suspend fun running() = OpenPak.status().running

    // ---- home ----

    private suspend fun showHome() {
        val status = OpenPak.status()
        val account = if (status.websiteSignedIn) OpenPak.callObject("account") else null
        val friends = if (status.websiteSignedIn) OpenPak.callObject("friends") else null
        val invitations = if (status.websiteSignedIn) OpenPak.callArray("invitations") else JSONArray()
        if (!begin(HOME)) return

        if (account != null) {
            identityCard(account, withActions = false)
        } else {
            val card = card()
            card.addView(text(getString(R.string.openpak_android_sign_in_card)))
            card.addView(button(getString(R.string.openpak_common_sign_in_button), enabled = status.running.isEmpty()) {
                OpenPakUi.showSignIn(requireActivity(), adopt = false) { if (it) show(HOME) }
            })
        }
        var incoming = 0
        friends?.optJSONArray("requests")?.forEachObject { if (it.optBoolean("incoming")) incoming++ }
        for (index in ACCOUNT..STATUS) {
            val badge = when (index) {
                FRIENDS -> incoming
                INVITATIONS -> invitations.length()
                else -> 0
            }
            navRow(ICONS[index]!!, getString(TITLES[index]!!), badge) { show(index) }
        }
        divider()
        navRow(R.drawable.ic_openpak_settings, getString(R.string.openpak_menu_settings), 0) { show(SETTINGS) }
        navRow(R.drawable.ic_openpak_website, getString(R.string.openpak_menu_website), 0) {
            OpenPakUi.openSite(requireActivity(), status.site)
        }
        if (status.websiteSignedIn && status.running.isEmpty()) {
            navRow(R.drawable.ic_openpak_sign_out, getString(R.string.openpak_menu_sign_out), 0) {
                OpenPakUi.confirmSignOut(requireActivity(), status.profileName) { show(HOME) }
            }
        }
    }

    // ---- Account ----

    private suspend fun needSignIn(index: Int): Boolean {
        val status = OpenPak.status()
        if (status.websiteSignedIn) return false
        if (!begin(index)) return true
        val panel = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(0, dp(48), 0, 0)
        }
        panel.addView(text(getString(R.string.openpak_common_need_sign_in)).apply { gravity = Gravity.CENTER })
        panel.addView(button(getString(R.string.openpak_common_sign_in_button), enabled = status.running.isEmpty()) {
            OpenPakUi.showSignIn(requireActivity(), adopt = false) { if (it) show(index) }
        })
        content.addView(panel)
        return true
    }

    private suspend fun showAccount() {
        if (needSignIn(ACCOUNT)) return
        val account = OpenPak.callObject("account")
        val status = OpenPak.status()
        if (!begin(ACCOUNT)) return
        identityCard(account, withActions = true, profileName = status.profileName, running = status.running.isNotEmpty())

        header(getString(R.string.openpak_menu_title))
        detail(getString(R.string.openpak_account_friend_code), account.optString("friend_code"))
        detail(
            getString(R.string.openpak_account_identity, "Switch"),
            account.optString("pid").takeIf { it.isNotEmpty() }?.let { getString(R.string.openpak_account_pid, it) } ?: ""
        )
        val platforms = mutableListOf<String>()
        account.optJSONArray("linked_platforms")?.let { for (i in 0 until it.length()) platforms += it.optString(i) }
        detail(getString(R.string.openpak_account_consoles), platforms.joinToString(", "))
        if (account.optBoolean("console_linked")) {
            detail(getString(R.string.openpak_account_link), getString(R.string.openpak_account_linked_as, account.optString("console_name")))
        } else {
            detail(getString(R.string.openpak_account_link), getString(R.string.openpak_account_link_failed))
            content.addView(button(getString(R.string.openpak_account_try_again), enabled = status.running.isEmpty()) {
                OpenPakUi.showSignIn(requireActivity(), adopt = false) { if (it) show(ACCOUNT) }
            })
        }
    }

    /** Avatar, name, friend code with Copy; on the Account screen also the change and sign-out actions. */
    private fun identityCard(account: JSONObject, withActions: Boolean, profileName: String = "", running: Boolean = false) {
        val card = card()
        val row = LinearLayout(requireContext()).apply { gravity = Gravity.CENTER_VERTICAL }
        row.addView(avatar(account.optString("avatar"), account.optString("name"), 56))
        val names = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(12), 0, 0, 0)
        }
        names.addView(text(account.optString("name"), 20f, bold = true))
        val code = account.optString("friend_code")
        names.addView(text(code.ifEmpty { getString(R.string.openpak_common_none) }).apply {
            typeface = Typeface.MONOSPACE
            setTextIsSelectable(true)
        })
        row.addView(names, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        card.addView(row)
        val actions = mutableListOf<Pair<String, () -> Unit>>()
        if (code.isNotEmpty()) actions += getString(R.string.openpak_common_copy) to { copy(code) }
        if (withActions) {
            actions += getString(R.string.openpak_account_change_name) to { changeName(account.optString("name")) }
            actions += getString(R.string.openpak_account_change_picture) to { pickPicture.launch("image/*") }
        }
        card.addView(buttonRow(actions))
        if (withActions) {
            card.addView(button(getString(R.string.openpak_menu_sign_out), enabled = !running) {
                OpenPakUi.confirmSignOut(requireActivity(), profileName) { show(HOME) }
            })
        }
    }

    private fun copy(code: String) {
        val clipboard = requireContext().getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
        if (clipboard == null) {
            say(getString(R.string.openpak_common_copy_failed, code))
            return
        }
        clipboard.setPrimaryClip(ClipData.newPlainText("OpenPak", code))
        say(getString(R.string.openpak_common_copied))
    }

    private fun changeName(current: String) {
        val field = EditText(requireActivity()).apply {
            setText(current)
            inputType = InputType.TYPE_CLASS_TEXT
            setSelectAllOnFocus(true)
        }
        val body = LinearLayout(requireContext()).apply { orientation = LinearLayout.VERTICAL }
        body.addView(field)
        body.addView(text(getString(R.string.openpak_account_name_rules), dim = true))
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(getString(R.string.openpak_account_change_name).trimEnd('.', '…'))
            .setView(OpenPakUi.frame(requireActivity(), body))
            .setPositiveButton(android.R.string.ok) { _, _ ->
                lifecycleScope.launch {
                    val error = OpenPak.action("set_username", JSONObject().put("name", field.text.toString().trim()))
                    say(error.ifEmpty { getString(R.string.openpak_account_name_updated) })
                    if (error.isEmpty() && isAdded) show(ACCOUNT)
                }
            }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    private fun changePicture(uri: Uri) {
        lifecycleScope.launch {
            val encoded = withContext(Dispatchers.IO) {
                runCatching {
                    val bytes = requireContext().contentResolver.openInputStream(uri)!!.use { it.readBytes() }
                    val source = BitmapFactory.decodeByteArray(bytes, 0, bytes.size)!!
                    val side = minOf(source.width, source.height)
                    val square = android.graphics.Bitmap.createBitmap(source, (source.width - side) / 2, (source.height - side) / 2, side, side)
                    val scaled = android.graphics.Bitmap.createScaledBitmap(square, 256, 256, true)
                    val out = ByteArrayOutputStream()
                    scaled.compress(android.graphics.Bitmap.CompressFormat.JPEG, 90, out)
                    Base64.encodeToString(out.toByteArray(), Base64.NO_WRAP)
                }.getOrNull()
            }
            if (encoded == null) {
                say(getString(R.string.openpak_error_image))
                return@launch
            }
            val error = OpenPak.action("set_picture", JSONObject().put("image", encoded))
            say(error.ifEmpty { getString(R.string.openpak_account_picture_updated) })
            if (error.isEmpty() && isAdded) show(ACCOUNT)
        }
    }

    // ---- Friends ----

    private suspend fun showFriends() {
        if (needSignIn(FRIENDS)) return
        val list = OpenPak.callObject("friends")
        if (!begin(FRIENDS)) return
        val field = EditText(requireActivity()).apply {
            hint = getString(R.string.openpak_friends_add_placeholder)
            inputType = InputType.TYPE_CLASS_TEXT
            imeOptions = android.view.inputmethod.EditorInfo.IME_ACTION_DONE
        }
        val add = {
            val code = field.text.toString().trim()
            if (code.isEmpty()) {
                say(getString(R.string.openpak_friends_no_code))
            } else {
                friendAction("add", JSONObject().put("code", code))
            }
        }
        field.setOnEditorActionListener { _, _, _ ->
            add()
            true
        }
        val addRow = LinearLayout(requireContext()).apply { gravity = Gravity.CENTER_VERTICAL }
        addRow.addView(field, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        addRow.addView(button(getString(R.string.openpak_friends_add)) { add() })
        content.addView(addRow)
        if (!list.optBoolean("ok")) {
            say(list.optString("error"))
            return
        }
        val running = list.optString("running")
        val requests = list.optJSONArray("requests") ?: JSONArray()
        if (requests.length() > 0) {
            header(getString(R.string.openpak_friends_requests))
            requests.forEachObject { request ->
                val incoming = request.optBoolean("incoming")
                row(
                    request.optString("name"),
                    getString(if (incoming) R.string.openpak_friends_incoming else R.string.openpak_friends_outgoing)
                )
                content.addView(
                    buttonRow(
                        if (incoming) {
                            listOf(
                                getString(R.string.openpak_friends_accept) to { friendAction("accept", request) },
                                getString(R.string.openpak_friends_decline) to { friendAction("decline", request) }
                            )
                        } else {
                            listOf(getString(R.string.openpak_friends_cancel_request) to { friendAction("decline", request) })
                        }
                    )
                )
            }
        }
        header(getString(R.string.openpak_friends_list))
        val friends = mutableListOf<JSONObject>()
        (list.optJSONArray("friends") ?: JSONArray()).forEachObject { friends += it }
        if (friends.isEmpty()) text(getString(R.string.openpak_friends_empty), dim = true).also { content.addView(it) }
        friends.sortedWith(compareBy<JSONObject>({ !it.optBoolean("online") }, { it.optString("name").lowercase() }))
            .forEach { friend ->
                val game = friend.optString("game")
                val state = when {
                    !friend.optBoolean("online") -> getString(R.string.openpak_friends_offline)
                    game.isNotEmpty() -> getString(R.string.openpak_friends_playing, game)
                    friend.optString("title_id").isNotEmpty() -> getString(R.string.openpak_friends_playing_unknown)
                    else -> getString(R.string.openpak_friends_online)
                }
                val code = friend.optString("friend_code")
                val detail = if (code.isEmpty()) state else state + "\n" + getString(R.string.openpak_friends_code_line, code)
                row(friend.optString("name"), detail, dot = friend.optBoolean("online")) {
                    val actions = mutableListOf<Pair<String, () -> Unit>>()
                    if (running.isNotEmpty() && friend.optString("title_id").equals(running, ignoreCase = true)) {
                        actions += getString(R.string.openpak_friends_join) to { friendAction("join", friend) }
                    }
                    actions += getString(R.string.openpak_friends_remove) to {
                        confirm(
                            getString(R.string.openpak_friends_remove_title),
                            getString(R.string.openpak_friends_remove_confirm, friend.optString("name")),
                            getString(R.string.openpak_friends_remove)
                        ) { friendAction("remove", friend) }
                    }
                    actions += getString(R.string.openpak_friends_block) to {
                        confirm(
                            getString(R.string.openpak_friends_block_title, friend.optString("name")),
                            getString(R.string.openpak_friends_block_confirm),
                            getString(R.string.openpak_friends_block)
                        ) { friendAction("block", friend) }
                    }
                    choose(friend.optString("name"), actions)
                }
            }
    }

    private fun friendAction(action: String, target: JSONObject) {
        val args = JSONObject(target.toString()).put("action", action)
        lifecycleScope.launch {
            val error = OpenPak.action("friend_action", args)
            say(
                error.ifEmpty {
                    if (action == "add") getString(R.string.openpak_friends_added, target.optString("code")) else ""
                }
            )
            if (isAdded) show(FRIENDS, quiet = true)
        }
    }

    // ---- Invitations ----

    private suspend fun showInvitations() {
        if (needSignIn(INVITATIONS)) return
        val list = OpenPak.callArray("invitations")
        if (!begin(INVITATIONS)) return
        if (list.length() == 0) content.addView(text(getString(R.string.openpak_invites_empty), dim = true))
        list.forEachObject { invitation ->
            val lines = mutableListOf(getString(R.string.openpak_invites_from, invitation.optString("from")))
            invitation.optString("message").takeIf { it.isNotEmpty() }?.let { lines += "“$it”" }
            val expires = invitation.optLong("expires_at").takeIf { it > 0 }?.let { OpenPakUi.time(it) }
                ?: invitation.optString("expires_text").takeIf { it.isNotEmpty() }?.let { OpenPakUi.time(it) }
            expires?.let { lines += getString(R.string.openpak_invites_expires, it) }
            row(invitation.optString("game"), lines.joinToString("\n"))
            content.addView(
                buttonRow(
                    listOf(
                        getString(R.string.openpak_invites_join) to { joinInvitation(invitation) },
                        getString(R.string.openpak_invites_ignore) to { answerInvitation(invitation, "dismiss") }
                    )
                )
            )
        }
    }

    /** Join hands it to the running game; otherwise it starts the game, which is then offered it. */
    private fun joinInvitation(invitation: JSONObject) {
        if (invitation.optBoolean("joinable")) {
            answerInvitation(invitation, "join")
            return
        }
        val game = installed().firstOrNull { it.first == invitation.optString("title_id").uppercase() }
        if (game == null || invitation.optString("source") != "console") {
            say(getString(R.string.openpak_invites_not_installed, invitation.optString("game")))
            return
        }
        dismiss()
        startActivity(game.third.launchIntent)
    }

    private fun answerInvitation(invitation: JSONObject, action: String) {
        lifecycleScope.launch {
            val error = OpenPak.action(
                "invitation_action",
                JSONObject().put("id", invitation.optString("id")).put("source", invitation.optString("source"))
                    .put("action", action)
            )
            if (error.isNotEmpty()) {
                say(if (action == "join") getString(R.string.openpak_invite_handover_failed) else error)
            }
            if (isAdded) show(INVITATIONS, quiet = true)
        }
    }

    // ---- Cloud saves ----

    private suspend fun showCloudSaves() {
        if (needSignIn(SAVES)) return
        val saves = OpenPak.callObject("cloud_saves")
        if (!begin(SAVES)) return
        if (!saves.optBoolean("ok")) {
            say(saves.optString("error"))
            return
        }
        val running = saves.optString("running").uppercase()
        val library = installed().map { it.first }.toSet()
        val allowance = saves.optLong("allowance")
        content.addView(
            text(
                getString(
                    R.string.openpak_saves_usage,
                    size(saves.optLong("used")),
                    if (allowance > 0) size(allowance) else getString(R.string.openpak_common_none)
                ),
                dim = true
            )
        )
        content.addView(button(getString(R.string.openpak_saves_upload)) { uploadPicked() })
        val titles = saves.optJSONArray("titles") ?: JSONArray()
        if (titles.length() == 0) content.addView(text(getString(R.string.openpak_saves_empty), dim = true))
        titles.forEachObject { title ->
            val id = title.optString("title_id").uppercase()
            val name = title.optString("name")
            val versions = title.optJSONArray("versions") ?: JSONArray()
            val newest = if (versions.length() > 0) versions.getJSONObject(0) else null
            val lines = mutableListOf(getString(R.string.openpak_saves_size, size(title.optLong("size")), versions.length().toString()))
            newest?.let {
                lines += getString(
                    R.string.openpak_saves_version,
                    it.optInt("number").toString(),
                    it.optString("device").ifEmpty { getString(R.string.openpak_common_none) },
                    OpenPakUi.time(it.optString("saved_at"))
                )
            }
            val installedHere = id in library
            val localLine = localLine(title, installedHere)
            lines += localLine
            val conflict = title.optBoolean("conflict")
            row(name + if (conflict) "  ·  " + getString(R.string.openpak_saves_conflict) else "", lines.joinToString("\n"))
            val thisRunning = running.isNotEmpty() && running == id
            val args = JSONObject().put("title_id", id).put("newest", title.optInt("newest"))
            val actions = mutableListOf<Pair<String, () -> Unit>>()
            if (conflict) {
                actions += getString(R.string.openpak_saves_resolve) to {
                    val cloudLine = newest?.let {
                        getString(
                            R.string.openpak_saves_version,
                            it.optInt("number").toString(),
                            it.optString("device").ifEmpty { getString(R.string.openpak_common_none) },
                            OpenPakUi.time(it.optString("saved_at"))
                        )
                    } ?: ""
                    OpenPakUi.showConflict(requireActivity(), name, localLine, cloudLine, !thisRunning) { keepLocal ->
                        saveAction(if (keepLocal) "upload" else "download", args, name)
                    }
                }
            }
            actions += getString(R.string.openpak_saves_download) to {
                if (thisRunning) {
                    say(getString(R.string.openpak_common_stop_game_first))
                } else {
                    confirm(null, getString(R.string.openpak_saves_download_confirm, name), getString(R.string.openpak_saves_download)) {
                        saveAction("download", args, name)
                    }
                }
            }
            if (installedHere) actions += getString(R.string.openpak_saves_upload) to { saveAction("upload", args, name) }
            actions += getString(R.string.openpak_saves_delete) to {
                confirm(null, getString(R.string.openpak_saves_delete_confirm, name), getString(R.string.openpak_saves_delete)) {
                    saveAction("delete", args, name)
                }
            }
            content.addView(buttonRow(actions))
        }
    }

    private fun localLine(title: JSONObject, installedHere: Boolean): String {
        val written = OpenPakUi.time(title.optLong("local_written"))
        return when (title.optString("local")) {
            "no_local" -> getString(if (installedHere) R.string.openpak_saves_local_none else R.string.openpak_saves_not_installed)
            "no_history" -> getString(R.string.openpak_saves_local_never, written)
            else -> getString(R.string.openpak_saves_local_synced, written, title.optString("local_version"))
        }
    }

    private fun uploadPicked() {
        val games = installed()
        if (games.isEmpty()) {
            say(getString(R.string.openpak_android_no_games))
            return
        }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_saves_upload)
            .setItems(games.map { it.second }.toTypedArray()) { _, which ->
                saveAction("upload", JSONObject().put("title_id", games[which].first).put("newest", 0), games[which].second)
            }
            .show()
    }

    private fun saveAction(action: String, args: JSONObject, name: String) {
        lifecycleScope.launch {
            val error = OpenPak.action("cloud_save_action", JSONObject(args.toString()).put("action", action))
            say(
                error.ifEmpty {
                    getString(
                        when (action) {
                            "download" -> R.string.openpak_saves_downloaded
                            "upload" -> R.string.openpak_saves_uploaded
                            else -> R.string.openpak_saves_deleted
                        },
                        name
                    )
                }
            )
            if (isAdded) show(SAVES, quiet = true)
        }
    }

    // ---- Mods ----

    private suspend fun showMods() {
        val title = chosenTitle ?: defaultTitle()
        val running = running().uppercase()
        val mods = title?.let { OpenPak.callArray("mods", JSONObject().put("title_id", it.first)) } ?: JSONArray()
        if (!begin(MODS)) return
        titlePicker(title)
        if (title == null) return
        if (mods.length() == 0) content.addView(text(getString(R.string.openpak_mods_empty), dim = true))
        val thisRunning = running.isNotEmpty() && running == title.first
        mods.forEachObject { mod ->
            val favourite = mod.optBoolean("favourite")
            val lines = mutableListOf<String>()
            mod.optString("summary").takeIf { it.isNotEmpty() }?.let { lines += it }
            lines += getString(R.string.openpak_mods_by, mod.optString("author"), mod.optString("licence"))
            row("${mod.optString("name")} ${mod.optString("version")}", lines.joinToString("\n"))
            val args = JSONObject().put("title_id", title.first).put("mod_id", mod.optString("id"))
            val name = mod.optString("name")
            val actions = mutableListOf<Pair<String, () -> Unit>>()
            actions += ((if (favourite) "★ " else "☆ ") + getString(R.string.openpak_mods_favourite)) to {
                modAction(if (favourite) "unfavourite" else "favourite", args, name)
            }
            val installAction = {
                if (thisRunning) say(getString(R.string.openpak_common_stop_game_first)) else modAction("install", args, name)
            }
            if (mod.optBoolean("installed")) {
                actions += getString(R.string.openpak_mods_reinstall) to installAction
                actions += getString(R.string.openpak_mods_uninstall) to {
                    if (thisRunning) say(getString(R.string.openpak_common_stop_game_first)) else modAction("uninstall", args, name)
                }
            } else {
                actions += getString(R.string.openpak_mods_install) to installAction
            }
            content.addView(buttonRow(actions))
        }
    }

    private fun modAction(action: String, args: JSONObject, name: String) {
        lifecycleScope.launch {
            val error = OpenPak.action("mod_action", JSONObject(args.toString()).put("action", action))
            say(
                when {
                    error == "refused" -> getString(R.string.openpak_mods_refused, name)
                    error == "failed" -> getString(R.string.openpak_mods_install_failed, name)
                    error.isNotEmpty() -> error
                    action == "install" -> getString(R.string.openpak_mods_installed, name)
                    else -> ""
                }
            )
            if (isAdded) show(MODS, quiet = true)
        }
    }

    // ---- News ----

    private suspend fun showNews() {
        val title = chosenTitle ?: defaultTitle()
        val news = title?.let { OpenPak.callObject("news", JSONObject().put("title_id", it.first)) }
        if (!begin(NEWS)) return
        titlePicker(title)
        if (title == null || news == null) return
        val files = news.optJSONArray("files") ?: JSONArray()
        if (news.optString("valid_from").isNotEmpty()) {
            content.addView(text(getString(R.string.openpak_news_from, OpenPakUi.time(news.optString("valid_from"))), dim = true))
        }
        if (files.length() == 0) {
            content.addView(text(getString(R.string.openpak_news_empty), dim = true))
            return
        }
        // No folder picker on every Android the apps support: the files go under the app's own
        // external files, where a file manager or a computer finds them.
        content.addView(button(getString(R.string.openpak_news_save)) {
            lifecycleScope.launch {
                val folder = File(requireContext().getExternalFilesDir(null), "openpak-news/${title.first}")
                val saved = OpenPak.callObject("news_save", JSONObject().put("title_id", title.first).put("dir", folder.absolutePath))
                say(getString(R.string.openpak_news_saved, saved.optInt("written").toString(), folder.absolutePath))
            }
        })
        files.forEachObject { row(it.optString("path"), size(it.optLong("size"))) }
    }

    // ---- Status ----

    private suspend fun showStatus() {
        val status = OpenPak.callObject("network_status")
        if (!begin(STATUS)) return
        val verdict = card()
        verdict.addView(
            text(
                status.optString("headline").ifEmpty { getString(R.string.openpak_status_no_health, status.optString("url")) },
                18f,
                bold = true
            ).apply { setCompoundDrawablesRelativeWithIntrinsicBounds(dotDrawable(status.optString("state") == "ok"), null, null, null) }
        )
        status.optString("sub").takeIf { it.isNotEmpty() }?.let { verdict.addView(text(it, dim = true)) }
        verdict.addView(text(getString(R.string.openpak_status_refreshed, OpenPakUi.time(System.currentTimeMillis() / 1000)), dim = true))
        if (status.optBoolean("network_ok")) {
            verdict.addView(text(getString(R.string.openpak_status_players, status.optInt("players_online").toString())))
        }

        val services = status.optJSONArray("services") ?: JSONArray()
        if (services.length() > 0) {
            header(getString(R.string.openpak_status_services))
            services.forEachObject {
                val up = it.optBoolean("up")
                row(
                    it.optString("name"),
                    getString(if (up) R.string.openpak_status_up else R.string.openpak_status_down) + " · " +
                        getString(R.string.openpak_status_uptime, String.format("%.2f", it.optDouble("uptime")), it.optString("latency")),
                    dot = up
                )
            }
        }

        header(getString(R.string.openpak_status_session))
        val account = status.optString("account")
        detail(
            getString(R.string.openpak_status_account),
            if (account.isEmpty()) getString(R.string.openpak_status_signed_out) else getString(R.string.openpak_status_signed_in, account)
        )
        detail(
            getString(R.string.openpak_status_link),
            if (status.optBoolean("console_linked")) {
                getString(R.string.openpak_status_linked, status.optString("console_name"), status.optString("console_friend_code"))
            } else {
                getString(R.string.openpak_status_no_console_account)
            }
        )
        val presence = status.optString("presence")
        val state = Regex("\"state\"\\s*:\\s*\"([A-Z_]+)\"").find(presence)?.groupValues?.get(1)
        detail(
            getString(R.string.openpak_status_presence),
            if (state == null) getString(R.string.openpak_status_presence_none) else getString(R.string.openpak_status_presence_on, state)
        )
        detail(getString(R.string.openpak_status_nat), getString(R.string.openpak_status_not_tested))
        detail(getString(R.string.openpak_status_ping), getString(R.string.openpak_status_not_tested))

        val titles = status.optJSONArray("titles") ?: JSONArray()
        if (titles.length() > 0) {
            header(getString(R.string.openpak_status_players_heading))
            titles.forEachObject {
                detail(it.optString("name"), getString(R.string.openpak_status_players, it.optInt("players").toString()))
            }
        }
    }

    // ---- OpenPak settings (3.13) ----

    private suspend fun showSettings() {
        val status = OpenPak.status()
        val profiles = OpenPak.profiles()
        if (!begin(SETTINGS)) return
        val running = status.running.isNotEmpty()
        header(getString(R.string.openpak_settings_account))
        content.addView(MaterialSwitch(requireActivity()).apply {
            setText(R.string.openpak_settings_enable)
            isChecked = status.redirect
            isEnabled = !running
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on ->
                lifecycleScope.launch {
                    OpenPak.setEnabled(on)
                    if (isAdded) show(SETTINGS, quiet = true)
                }
            }
        })
        content.addView(text(getString(R.string.openpak_settings_enable_tip), dim = true))
        content.addView(
            text(
                if (status.websiteSignedIn) {
                    getString(R.string.openpak_settings_account_row, status.profileName, status.username)
                } else {
                    getString(R.string.openpak_settings_account_row_out, status.profileName)
                }
            )
        )
        if (status.websiteSignedIn) {
            content.addView(button(getString(R.string.openpak_menu_sign_out), enabled = !running) {
                OpenPakUi.confirmSignOut(requireActivity(), status.profileName) { show(SETTINGS) }
            })
        } else {
            content.addView(button(getString(R.string.openpak_common_sign_in_button), enabled = !running && status.redirect) {
                OpenPakUi.showSignIn(requireActivity(), adopt = false) { if (it) show(SETTINGS) }
            })
        }

        // Account at startup, and the profiles themselves: one OpenPak account per profile.
        val startup = OpenPak.startupProfile
        val startupLabel = when (startup) {
            "" -> getString(R.string.openpak_settings_startup_last)
            OpenPakUi.STARTUP_ASK -> getString(R.string.openpak_settings_startup_ask)
            else -> profiles.firstOrNull { it.uuid == startup }?.name ?: getString(R.string.openpak_settings_startup_last)
        }
        detail(getString(R.string.openpak_settings_startup), startupLabel) { chooseStartupProfile(profiles) }
        content.addView(text(getString(R.string.openpak_settings_startup_tip), dim = true))
        header(getString(R.string.openpak_android_profiles))
        profiles.forEach { profile ->
            row(
                profile.name + if (profile.current) "  ✓" else "",
                profile.account.ifEmpty { getString(R.string.openpak_picker_offline) }
            ) {
                if (running) {
                    say(getString(R.string.openpak_common_stop_game_first))
                    return@row
                }
                val actions = mutableListOf<Pair<String, () -> Unit>>()
                if (!profile.current) {
                    actions += getString(R.string.openpak_android_use_profile) to {
                        lifecycleScope.launch {
                            OpenPakUi.selectProfile(profile.uuid)
                            show(SETTINGS, quiet = true)
                        }
                    }
                }
                actions += getString(R.string.openpak_android_rename_profile) to { renameProfile(profile) }
                if (profiles.size > 1) {
                    actions += getString(R.string.openpak_android_delete_profile) to {
                        confirm(null, getString(R.string.openpak_android_delete_profile_confirm, profile.name), getString(R.string.openpak_android_delete_profile)) {
                            profileAction("remove", profile.uuid)
                        }
                    }
                }
                choose(profile.name, actions)
            }
        }
        content.addView(button(getString(R.string.openpak_picker_add), enabled = !running) {
            OpenPakUi.runSetup(requireActivity(), true) { if (isAdded) show(SETTINGS, quiet = true) }
        })
        content.addView(MaterialSwitch(requireActivity()).apply {
            setText(R.string.openpak_settings_cloud_sync)
            isChecked = OpenPak.cloudSyncEnabled
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on -> OpenPak.cloudSyncEnabled = on }
        })

        header(getString(R.string.openpak_settings_notifications_heading))
        content.addView(MaterialSwitch(requireActivity()).apply {
            setText(R.string.openpak_settings_notifications)
            isChecked = OpenPak.notificationsEnabled
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on -> OpenPak.notificationsEnabled = on }
        })

        header(getString(R.string.openpak_settings_advanced))
        detail(getString(R.string.openpak_settings_website), status.site)
        content.addView(text(getString(R.string.openpak_settings_website_tip), dim = true))
    }

    private fun chooseStartupProfile(profiles: List<OpenPak.Profile>) {
        val values = listOf("", OpenPakUi.STARTUP_ASK) + profiles.map { it.uuid }
        val labels = listOf(getString(R.string.openpak_settings_startup_last), getString(R.string.openpak_settings_startup_ask)) +
            profiles.map { it.name }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_settings_startup)
            .setSingleChoiceItems(labels.toTypedArray(), values.indexOf(OpenPak.startupProfile).coerceAtLeast(0)) { dialog, which ->
                OpenPak.startupProfile = values[which]
                dialog.dismiss()
                show(SETTINGS, quiet = true)
            }
            .show()
    }

    private fun renameProfile(profile: OpenPak.Profile) {
        val name = EditText(requireActivity()).apply {
            setText(profile.name)
            inputType = InputType.TYPE_CLASS_TEXT
            setSelectAllOnFocus(true)
        }
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(R.string.openpak_setup_profile_name)
            .setView(OpenPakUi.frame(requireActivity(), name))
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val chosen = name.text.toString().trim().take(32)
                if (chosen.isNotEmpty()) profileAction("rename", profile.uuid, chosen)
            }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    private fun profileAction(action: String, uuid: String, name: String = "") {
        lifecycleScope.launch {
            val error = OpenPak.profileAction(action, uuid, name).optString("error")
            say(error)
            withContext(Dispatchers.IO) { org.citron.citron_emu.utils.NativeConfig.saveGlobalConfig() }
            if (isAdded) show(SETTINGS, quiet = true)
        }
    }

    // ---- titles: the library, else the catalogue ----

    /** The installed games: 16 hex digits, name, and the game itself (to start it). */
    private fun installed(): List<Triple<String, String, Game>> =
        gamesViewModel.games.value
            .filter { !it.isHomebrew && (it.programId.toLongOrNull() ?: 0L) != 0L }
            .map { Triple(java.lang.Long.toHexString(it.programId.toLong()).padStart(16, '0').uppercase(), it.title, it) }
            .distinctBy { it.first }
            .sortedBy { it.second.lowercase() }

    private suspend fun titles(): List<Pair<String, String>> {
        val library = installed().map { it.first to it.second }
        if (library.isNotEmpty()) return library
        val out = mutableListOf<Pair<String, String>>()
        OpenPak.callArray("catalogue").forEachObject { out += it.optString("title_id").uppercase() to it.optString("name") }
        return out.sortedBy { it.second.lowercase() }
    }

    /** The running game when there is one (spec: Mods defaults to it), else the first title. */
    private suspend fun defaultTitle(): Pair<String, String>? {
        val all = titles()
        val running = running().uppercase()
        chosenTitle = all.firstOrNull { it.first == running } ?: all.firstOrNull()
        return chosenTitle
    }

    private fun titlePicker(title: Pair<String, String>?) {
        detail(getString(R.string.openpak_android_choose_game), (title?.second ?: getString(R.string.openpak_android_no_games)) + "  ▾") {
            lifecycleScope.launch {
                val all = titles()
                if (all.isEmpty() || !isAdded) return@launch
                MaterialAlertDialogBuilder(requireActivity())
                    .setTitle(R.string.openpak_android_choose_game)
                    .setItems(all.map { it.second }.toTypedArray()) { _, which ->
                        chosenTitle = all[which]
                        show(page)
                    }
                    .show()
            }
        }
    }

    // ---- the row kit ----

    private fun dp(value: Int) = OpenPakUi.dp(requireContext(), value)

    private fun text(value: String, size: Float = 14f, bold: Boolean = false, dim: Boolean = false) =
        OpenPakUi.text(requireContext(), value, size, bold, dim)

    private fun header(value: String) {
        content.addView(text(value, 18f, bold = true).apply { setPadding(0, dp(16), 0, dp(4)) })
    }

    private fun divider() {
        content.addView(View(requireContext()).apply {
            setBackgroundColor(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOutlineVariant, 0x22000000))
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1)).apply { setMargins(0, dp(8), 0, dp(8)) })
    }

    private fun card(): LinearLayout {
        val inner = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(16), dp(16), dp(16), dp(16))
        }
        content.addView(MaterialCardView(requireActivity()).apply { addView(inner) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(8) })
        return inner
    }

    private fun clickable(view: View, onClick: () -> Unit) {
        val outValue = android.util.TypedValue()
        view.context.theme.resolveAttribute(android.R.attr.selectableItemBackground, outValue, true)
        view.setBackgroundResource(outValue.resourceId)
        view.isClickable = true
        view.isFocusable = true
        view.setOnClickListener { onClick() }
    }

    /** A home row: icon, title, a count badge when there is something waiting, and a chevron. */
    private fun navRow(icon: Int, title: String, badge: Int, onClick: () -> Unit) {
        val row = LinearLayout(requireContext()).apply {
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(56)
            setPadding(dp(8), 0, dp(8), 0)
        }
        row.addView(ImageView(requireContext()).apply { setImageResource(icon) }, LinearLayout.LayoutParams(dp(24), dp(24)))
        row.addView(text(title, 16f).apply { setPadding(dp(16), 0, 0, 0) }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (badge > 0) {
            row.addView(TextView(requireContext()).apply {
                text = if (badge > 99) "99+" else badge.toString()
                setTextColor(0xFFFFFFFF.toInt())
                textSize = 12f
                setPadding(dp(8), dp(2), dp(8), dp(2))
                background = android.graphics.drawable.GradientDrawable().apply {
                    cornerRadius = dp(12).toFloat()
                    setColor(0xFFD32F2F.toInt())
                }
            })
        }
        row.addView(ImageView(requireContext()).apply { setImageResource(R.drawable.ic_openpak_chevron) }, LinearLayout.LayoutParams(dp(24), dp(24)))
        clickable(row, onClick)
        content.addView(row)
    }

    /** A row: the name (with a presence dot when given) and its lines; tapping opens its actions. */
    private fun row(title: String, detail: String, dot: Boolean? = null, onClick: (() -> Unit)? = null) {
        val row = LinearLayout(requireContext()).apply {
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = dp(48)
            setPadding(0, dp(8), 0, dp(8))
        }
        val lines = LinearLayout(requireContext()).apply { orientation = LinearLayout.VERTICAL }
        lines.addView(text(title, 16f).apply {
            if (dot != null) setCompoundDrawablesRelativeWithIntrinsicBounds(dotDrawable(dot), null, null, null)
            compoundDrawablePadding = dp(8)
        })
        if (detail.isNotEmpty()) lines.addView(text(detail, 13f, dim = true))
        row.addView(lines, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        if (onClick != null) {
            row.addView(ImageView(requireContext()).apply {
                setImageResource(R.drawable.ic_openpak_more)
                contentDescription = title
            }, LinearLayout.LayoutParams(dp(24), dp(24)))
            clickable(row, onClick)
        }
        content.addView(row)
    }

    /** "Label: value", the desktop's detail rows; an empty value reads "—". */
    private fun detail(label: String, value: String, onClick: (() -> Unit)? = null) {
        val row = LinearLayout(requireContext()).apply {
            orientation = LinearLayout.VERTICAL
            minimumHeight = dp(48)
            setPadding(0, dp(6), 0, dp(6))
        }
        row.addView(text(label, 13f, dim = true))
        row.addView(text(value.ifEmpty { getString(R.string.openpak_common_none) }, 15f))
        if (onClick != null) clickable(row, onClick)
        content.addView(row)
    }

    private fun dotDrawable(on: Boolean) = android.graphics.drawable.GradientDrawable().apply {
        shape = android.graphics.drawable.GradientDrawable.OVAL
        setColor(if (on) 0xFF3FB950.toInt() else 0xFF7A7A7A.toInt())
        setSize(dp(10), dp(10))
    }

    private fun avatar(base64: String, name: String, sizeDp: Int): View {
        val image = ShapeableImageView(requireContext()).apply {
            shapeAppearanceModel = ShapeAppearanceModel.builder().setAllCornerSizes(dp(sizeDp) / 2f).build()
            scaleType = ImageView.ScaleType.CENTER_CROP
        }
        val bitmap = runCatching {
            val bytes = Base64.decode(base64, Base64.DEFAULT)
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size)
        }.getOrNull()
        if (bitmap != null) {
            image.setImageBitmap(bitmap)
            image.layoutParams = LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp))
            return image
        }
        // Initials on a tinted circle until there is a picture.
        return TextView(requireContext()).apply {
            text = name.split(' ').filter { it.isNotEmpty() }.take(2).joinToString("") { it.take(1).uppercase() }.ifEmpty { "?" }
            gravity = Gravity.CENTER
            textSize = 20f
            setTextColor(0xFFFFFFFF.toInt())
            background = android.graphics.drawable.GradientDrawable().apply {
                shape = android.graphics.drawable.GradientDrawable.OVAL
                setColor(0xFF4F46E5.toInt())
            }
            layoutParams = LinearLayout.LayoutParams(dp(sizeDp), dp(sizeDp))
        }
    }

    private fun button(label: String, enabled: Boolean = true, action: () -> Unit) =
        MaterialButton(requireActivity(), null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
            text = label
            isAllCaps = false
            isEnabled = enabled
            minHeight = dp(48)
            setOnClickListener { action() }
        }

    private fun buttonRow(buttons: List<Pair<String, () -> Unit>>): View {
        val bar = com.google.android.material.chip.ChipGroup(requireContext())
        buttons.forEach { (label, action) -> bar.addView(button(label, action = action)) }
        return bar
    }

    private fun choose(title: String, actions: List<Pair<String, () -> Unit>>) {
        MaterialAlertDialogBuilder(requireActivity())
            .setTitle(title)
            .setItems(actions.map { it.first }.toTypedArray()) { _, which -> actions[which].second() }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    /** A destructive action's confirmation: Cancel is the safe answer. */
    private fun confirm(title: String?, message: String, confirmLabel: String, action: () -> Unit) {
        MaterialAlertDialogBuilder(requireActivity())
            .apply { if (title != null) setTitle(title) }
            .setMessage(message)
            .setPositiveButton(confirmLabel) { _, _ -> action() }
            .setNegativeButton(R.string.openpak_common_cancel, null)
            .show()
    }

    private fun size(bytes: Long): String = Formatter.formatShortFileSize(requireContext(), bytes)

    private inline fun JSONArray.forEachObject(action: (JSONObject) -> Unit) {
        for (index in 0 until length()) action(getJSONObject(index))
    }

    companion object {
        const val TAG = "OpenPakFragment"
        private const val KEY_PAGE = "page"

        const val HOME = -1
        const val ACCOUNT = 0
        const val FRIENDS = 1
        const val INVITATIONS = 2
        const val SAVES = 3
        const val MODS = 4
        const val NEWS = 5
        const val STATUS = 6
        const val SETTINGS = 7

        private val TITLES = mapOf(
            HOME to R.string.openpak_menu_title,
            ACCOUNT to R.string.openpak_page_account,
            FRIENDS to R.string.openpak_page_friends,
            INVITATIONS to R.string.openpak_page_invitations,
            SAVES to R.string.openpak_page_saves,
            MODS to R.string.openpak_page_mods,
            NEWS to R.string.openpak_page_news,
            STATUS to R.string.openpak_page_status,
            SETTINGS to R.string.openpak_settings_section
        )

        private val ICONS = mapOf(
            ACCOUNT to R.drawable.ic_openpak_account,
            FRIENDS to R.drawable.ic_openpak_friends,
            INVITATIONS to R.drawable.ic_openpak_invitations,
            SAVES to R.drawable.ic_openpak_saves,
            MODS to R.drawable.ic_openpak_mods,
            NEWS to R.drawable.ic_openpak_news,
            STATUS to R.drawable.ic_openpak_status
        )

        fun newInstance(page: Int = HOME) = OpenPakFragment().apply {
            arguments = Bundle().apply { putInt(KEY_PAGE, page) }
        }
    }
}
