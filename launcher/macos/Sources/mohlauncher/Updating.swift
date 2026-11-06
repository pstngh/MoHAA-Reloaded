// The update's flow in the window, as the Windows and Linux launcher's (launcher/desktop/src/main.rs): the check at
// start, the offer, the progress with Cancel, the result. LauncherCore's Update.swift does the work.
import AppKit
import LauncherCore
import SwiftUI

enum UpdateDialog: Equatable {
    case none
    /// A newer build is out: update?
    case offer
    /// Its progress, with Cancel until the files move
    case progress
    /// How it ended. `releasePage`: an error, with the button to the download
    case message(title: String, text: String, releasePage: Bool)
}

extension LauncherModel {
    private var launcherBundle: URL { Bundle.main.bundleURL }

    /// At every start: out of a temporary copy if macOS runs one, what an earlier update left put right, then the check
    func startUpdater() {
        let arguments = CommandLine.arguments
        // CI's picture of the window
        if arguments.contains("--snapshot") {
            updaterOff = true
            return
        }
        if let k = arguments.firstIndex(of: "--updated"), k + 1 < arguments.count {
            showUpdated(commit: arguments[k + 1])
        } else if arguments.contains("--update-undone") {
            showUpdateFailure(title: "The update didn't finish", text: "The update was interrupted, so the game's files are as they were.")
        }
        if case .yes(let original) = translocation(of: launcherBundle) {
            leaveTranslocation(original: original)
            return
        }
        Task { @MainActor in
            // Moved in the Finder and opened with Open Anyway: not a temporary copy, but the game beside it would stop
            // at Gatekeeper. What install.command does, for the download's items.
            if let folder = updateFolder(launcherBundle: launcherBundle) {
                let items = installedItems(folder: folder)
                await Task.detached { try? clearQuarantine(items) }.value
            }
            runRecovery()
        }
    }

    /// Open Anyway runs the launcher from a read-only copy, where it can't update anything or find the game beside it.
    /// As install.command does: the quarantine off its real folder's items, none left, then a start from there. If
    /// that fails the player hears why and no update is offered; a second start from there doesn't try again.
    private func leaveTranslocation(original: URL?) {
        guard let original, !CommandLine.arguments.contains("--left-translocation") else {
            refuseTranslocated()
            return
        }
        Task { @MainActor in
            let items = installedItems(folder: original.deletingLastPathComponent()).filter {
                $0.lastPathComponent != UpdateNames.launcher
            } + [original]
            let cleared = await Task.detached { () -> Bool in
                (try? clearQuarantine(items)) != nil
            }.value
            guard cleared else {
                refuseTranslocated()
                return
            }
            let configuration = NSWorkspace.OpenConfiguration()
            configuration.createsNewApplicationInstance = true
            configuration.arguments = ["--left-translocation"]
            NSWorkspace.shared.openApplication(at: original, configuration: configuration) { _, error in
                let started = error == nil
                Task { @MainActor in
                    if started {
                        NSApp.terminate(nil)
                    } else {
                        self.refuseTranslocated()
                    }
                }
            }
        }
    }

    private func refuseTranslocated() {
        updaterOff = true
        updateBusy = false
        showUpdateFailure(error: .translocated)
    }

    /// An earlier update's journal: finished or undone, before anything else happens to the folder. The game can't
    /// start until it's done. Tried again by `updaterTick` while another launcher holds the folder (the one that just
    /// started this one, quitting) or a game runs from it.
    func runRecovery() {
        guard !updaterOff, !recovering, !updating else { return }
        if let wait = nextRecovery, wait > Date() { return }
        // The launcher's own folder, whatever is in it: an interrupted update may have one of the apps away, or this
        // may be the old launcher under the name it waits by. There's nothing to do unless the work folder is there.
        let folder = launcherBundle.deletingLastPathComponent()
        let lock: UpdateLock
        switch UpdateLock.take(folder: folder) {
        case .locked(let taken):
            lock = taken
        case .busy:
            recoveryPending = true
            return
        case .noAccess:
            finishRecovery(.noAccess, folder: folder)
            return
        }
        recovering = true
        Task { @MainActor in
            let outcome = await Task.detached { () -> RecoveryOutcome in
                let installer = UpdateInstaller(folder: folder)
                return (try? installer.recover(gameRunning: { gameIsRunning(folder: folder) })) ?? .stuck
            }.value
            lock.release()
            recovering = false
            finishRecovery(outcome, folder: folder)
        }
    }

    private func finishRecovery(_ outcome: RecoveryOutcome, folder: URL) {
        recoveryPending = false
        findGame() // an app that was away may be back
        switch outcome {
        case .gameRunning:
            recoveryPending = true
            return
        case .stuck:
            // A half pair isn't for starting. Tried again now and then: the player may put fresh apps in (the message
            // says so), and that clears it. Said once.
            updateBusy = true
            updateStuck = true
            recoveryPending = true
            nextRecovery = Date().addingTimeInterval(15)
            if !stuckShown {
                stuckShown = true
                showUpdateFailure(error: .interrupted)
            }
            return
        case .noAccess:
            // macOS's folder access was refused: nothing can be known, and the game can't run from there either
            updateBusy = false
            updaterOff = true
            return
        case .finished(let commit):
            if !CommandLine.arguments.contains("--updated") { showUpdated(commit: commit) }
        case .undone(let launcherReplaced):
            if launcherReplaced {
                // the bundle this one came from was just swapped out: the right one takes over
                restartLauncher(arguments: ["--update-undone"], folder: folder,
                                ifNot: "The update was undone: open MoHAA Launcher again.")
                return
            }
            showUpdateFailure(title: "The update didn't finish", text: "The update was interrupted, so the game's files are as they were.")
        case .nothing, .cleaned:
            break
        }
        updateStuck = false
        nextRecovery = nil
        updateBusy = false
        guard let pair = updateFolder(launcherBundle: launcherBundle) else {
            // not the pair side by side: nothing to offer
            updaterOff = true
            return
        }
        checkForNewerBuild(folder: pair)
    }

    /// Asks the release once per start. With no network or no answer, nothing is said.
    private func checkForNewerBuild(folder: URL) {
        guard !updateChecked else { return }
        updateChecked = true
        let fetch = updateFetch(folder: folder)
        Task { @MainActor in
            let offer = await Task.detached { () -> UpdateOffer? in
                await checkForUpdate(installed: installedCommit(folder: folder), fetch: fetch)
            }.value
            updateOffer = offer
            offerUpdate()
        }
    }

    /// How GitHub is asked. A test build may answer from files saved in the folder (mktest.sh makes them with a token):
    /// GitHub allows 60 calls an hour without one, and a day of tests uses them up.
    private func updateFetch(folder: URL) -> UpdateFetch {
        #if MOHL_TEST_HOOKS
        let saved = folder.appendingPathComponent(".mohl-test-github")
        if FileManager.default.fileExists(atPath: saved.path) {
            return { url in
                let path = url.path
                let name = path.hasSuffix("/releases/tags/\(UpdateNames.tag)") ? "release"
                    : path.hasSuffix("/git/ref/tags/\(UpdateNames.tag)") ? "ref"
                    : path.contains("/git/tags/") ? "tag" : "compare"
                return (200, try Data(contentsOf: saved.appendingPathComponent("\(name).json")))
            }
        }
        #endif
        return { try await githubFetch($0) }
    }

    /// The newer build found, offered when nothing stands in the way: no other question open, and the game not running
    func offerUpdate() {
        guard updateOffer != nil, !updateDeclined, updateDialog == .none, !running, !starting, !askForFolder, alertMessage == nil,
              let folder = updateFolder(launcherBundle: launcherBundle), !gameIsRunning(folder: folder) else { return }
        updateDialog = .offer
    }

    /// From the model's tick
    func updaterTick() {
        if recoveryPending {
            runRecovery()
        } else if updateDialog == .none, !updating {
            offerUpdate()
        }
    }

    func declineUpdate() {
        updateDeclined = true
        updateDialog = .none
    }

    /// Update: it downloads and installs off the window's thread, the dialog shows how far it has got
    func startUpdate() {
        guard let stale = updateOffer, let folder = updateFolder(launcherBundle: launcherBundle) else {
            updateDialog = .none
            return
        }
        // another game in the folder since the offer: it may be newer than what's offered
        guard installedCommit(folder: folder) == stale.installed else {
            updateOffer = nil
            updateDialog = .none
            return
        }
        if running || gameIsRunning(folder: folder) {
            showUpdateFailure(title: "Close the game first", text: UpdateError.gameRunning.message)
            return
        }
        switch UpdateLock.take(folder: folder) {
        case .locked(let lock):
            updateLock = lock
        case .busy:
            showUpdateFailure(error: .busy)
            return
        case .noAccess:
            showUpdateFailure(error: .unwritable)
            return
        }
        let flags = UpdateFlags()
        updateFlags = flags
        updating = true
        updateBusy = true
        updateProgress = 0
        updateCanCancel = true
        updateText = "Downloading \(stale.asset)…"
        updateDialog = .progress
        let fetch = updateFetch(folder: folder)
        var hooks = UpdateHooks()
        #if MOHL_TEST_HOOKS
        let slow = LauncherModel.testHooks(&hooks, folder: folder)
        #else
        let slow: UInt32 = 0
        #endif
        Task { @MainActor in
            // The offer may be hours old, and CI may have moved on. With no answer the old one goes on: offline that's
            // the network's message, and against a replaced zip the size or the hash says try later. GitHub saying it's
            // not newer any more ends it.
            let fresh = await Task.detached { () -> UpdateCheck in
                await checkRelease(installed: stale.installed, fetch: fetch)
            }.value
            let offer: UpdateOffer
            switch fresh {
            case .newer(let newer): offer = newer
            case .noAnswer: offer = stale
            case .notNewer:
                finishUpdate(.failure(UpdateError.cancelled), offer: stale, folder: folder)
                updateOffer = nil
                return
            }
            let frozenHooks = hooks
            let result = await Task.detached { () -> Result<Bool, Error> in
                do {
                    return .success(try await runUpdate(offer, folder: folder, flags: flags, hooks: frozenHooks) { stage in
                        if slow > 0, case .download = stage { usleep(slow) }
                        Task { @MainActor in self.showUpdateStage(stage, offer: offer) }
                    })
                } catch {
                    return .failure(error)
                }
            }.value
            finishUpdate(result, offer: offer, folder: folder)
        }
    }

    private func showUpdateStage(_ stage: UpdateStage, offer: UpdateOffer) {
        guard updating, updateDialog == .progress else { return }
        // after Cancel, only the files moving (it came too late) is news
        if updateFlags?.stopped == true, case .install = stage {} else if updateFlags?.stopped == true { return }
        switch stage {
        case .download(let done):
            let mb = { (bytes: Int64) in String(format: "%.1f", Double(bytes) / 1e6) }
            updateText = "Downloading \(offer.asset): \(mb(done)) of \(mb(offer.size)) MB"
            updateProgress = Double(done) / Double(max(offer.size, 1))
        case .unpack:
            updateText = "Unpacking and checking \(offer.asset)…"
            updateProgress = 1
        case .install:
            updateText = "Putting the new files in place…"
            updateCanCancel = false
        }
    }

    /// Cancel: the update stops and the dialog closes when it has, unless its files are moving already
    func cancelUpdate() {
        guard let flags = updateFlags else { return }
        updateText = flags.stop() ? "Cancelling…" : "Putting the new files in place…"
        updateCanCancel = false
    }

    private func finishUpdate(_ result: Result<Bool, Error>, offer: UpdateOffer, folder: URL) {
        updating = false
        updateFlags = nil
        updateBusy = false
        // the new launcher looks at the folder as soon as it starts
        updateLock?.release()
        updateLock = nil
        updateDialog = .none
        if quitAfterUpdate {
            NSApp.reply(toApplicationShouldTerminate: true)
            return
        }
        switch result {
        case .success(let replacedLauncher):
            updateOffer = nil
            findGame()
            if replacedLauncher {
                restartLauncher(arguments: ["--updated", offer.commit], folder: folder,
                                ifNot: "MoHAA Reloaded is updated: open MoHAA Launcher again.")
            } else {
                showUpdated(commit: offer.commit)
            }
        case .failure(let error):
            let reason = error as? UpdateError ?? updateError(from: error)
            switch reason {
            case .cancelled:
                updateDeclined = true
            case .gameRunning:
                showUpdateFailure(title: "Close the game first", text: reason.message)
            case .interrupted:
                updateOffer = nil
                recoveryPending = true
                updateBusy = true
                showUpdateFailure(error: reason)
            default:
                updateOffer = nil
                showUpdateFailure(error: reason)
            }
        }
    }

    /// The new launcher in this one's place, and this one gone. It's a new instance: the running one has this bundle
    /// identifier too, and without that Launch Services would only bring it to the front.
    private func restartLauncher(arguments: [String], folder: URL, ifNot message: String) {
        save()
        let configuration = NSWorkspace.OpenConfiguration()
        configuration.createsNewApplicationInstance = true
        configuration.arguments = arguments
        let app = folder.appendingPathComponent(UpdateNames.launcher)
        NSWorkspace.shared.openApplication(at: app, configuration: configuration) { _, error in
            let started = error == nil
            Task { @MainActor in
                if started {
                    NSApp.terminate(nil)
                } else {
                    self.updateBusy = false
                    self.showUpdateMessage(title: "MoHAA Launcher", text: message)
                }
            }
        }
    }

    func showUpdated(commit: String) {
        showUpdateMessage(title: "Updated", text: "MoHAA Reloaded is up to date: the game's files are the newest build's, \(commit.prefix(8)).")
    }

    private func showUpdateMessage(title: String, text: String) {
        updateDialog = .message(title: title, text: text, releasePage: false)
    }

    func showUpdateFailure(error: UpdateError) {
        showUpdateFailure(title: "The update didn't finish", text: error.message)
    }

    private func showUpdateFailure(title: String, text: String) {
        updateDialog = .message(title: title, text: text, releasePage: true)
    }

    /// OK: the question about the folder, if one was waiting for this dialog (one sheet or alert at a time)
    func closeUpdateMessage() {
        updateDialog = .none
        if folder == nil, !recovering { askForFolder = true }
    }

    func openReleasePage() {
        NSWorkspace.shared.open(UpdateNames.releasePage)
    }

    /// Quitting: a download just stops (the next start removes it), files that are moving get to finish
    func shouldTerminate() -> NSApplication.TerminateReply {
        guard updating, let flags = updateFlags else { return .terminateNow }
        if flags.stop() { return .terminateNow }
        quitAfterUpdate = true
        return .terminateLater
    }

    #if MOHL_TEST_HOOKS
    /// A test build's hooks (bundle.sh with MOHL_TEST_HOOKS=1; release.sh refuses a launcher that has them), from
    /// mohl-test-hooks.txt in the game folder, a line each: corrupt-zip, kill-at <step>, pause-at <step> <seconds>,
    /// slow-download <ms per chunk>, use-launcher <bundle in the folder> (it goes in the download's launcher's place:
    /// today's release has no updater to take over), keep-launcher (as if the source were the same), download-url <url>
    /// (a local address that refuses or breaks the connection, with the network left on). The steps are
    /// UpdateInstaller's failpoints. Returns the download's delay in µs.
    static func testHooks(_ hooks: inout UpdateHooks, folder: URL) -> UInt32 {
        let file = folder.appendingPathComponent("mohl-test-hooks.txt")
        guard let text = try? String(contentsOf: file, encoding: .utf8) else { return 0 }
        var slow: UInt32 = 0
        var kills: Set<String> = []
        var pauses: [String: UInt32] = [:]
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: " ").map(String.init)
            switch parts.first {
            case "corrupt-zip":
                hooks.afterDownload = { url in
                    guard let handle = try? FileHandle(forUpdating: url) else { return }
                    try? handle.seek(toOffset: 1000)
                    try? handle.write(contentsOf: Data([0xFF, 0x00, 0xFF, 0x00]))
                    try? handle.close()
                }
            case "use-launcher" where parts.count == 2:
                let source = folder.appendingPathComponent(parts[1])
                let previous = hooks.beforeInstall
                hooks.beforeInstall = { staged in
                    previous?(&staged)
                    let target = staged.stage.appendingPathComponent(UpdateNames.launcher)
                    try? FileManager.default.removeItem(at: target)
                    try? FileManager.default.copyItem(at: source, to: target)
                }
            case "download-url" where parts.count == 2:
                hooks.downloadURL = URL(string: parts[1])
            case "keep-launcher":
                let previous = hooks.beforeInstall
                hooks.beforeInstall = { staged in
                    previous?(&staged)
                    staged.replaceLauncher = false
                }
            case "kill-at" where parts.count == 2: kills.insert(parts[1])
            case "pause-at" where parts.count == 3: pauses[parts[1]] = UInt32(parts[2]) ?? 0
            case "slow-download" where parts.count == 2: slow = (UInt32(parts[1]) ?? 0) * 1000
            default: break
            }
        }
        hooks.failpoint = { step in
            if let seconds = pauses[step] {
                NSLog("mohlauncher test hook: pausing %u s before %@", seconds, step)
                sleep(seconds)
            }
            if kills.contains(step) {
                NSLog("mohlauncher test hook: killed before %@", step)
                kill(getpid(), SIGKILL)
            }
        }
        return slow
    }
    #endif
}

/// The update's dialog: the offer, the progress, the result
struct UpdateSheet: View {
    @EnvironmentObject var model: LauncherModel

    private var title: String {
        switch model.updateDialog {
        case .offer: return "Update available"
        case .progress: return "Updating"
        case .message(let title, _, _): return title
        case .none: return ""
        }
    }

    private var text: String {
        switch model.updateDialog {
        case .offer: return "A newer version of MoHAA Reloaded is out. Update now?"
        case .progress: return model.updateText
        case .message(_, let text, _): return text
        case .none: return ""
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Metrics.gap) {
            Text(title).font(.headline)
            Text(text).fixedSize(horizontal: false, vertical: true)
            if model.updateDialog == .progress {
                ProgressView(value: model.updateProgress)
            }
            HStack(spacing: 8) {
                Spacer()
                switch model.updateDialog {
                case .offer:
                    Button("Not Now") { model.declineUpdate() }
                        .keyboardShortcut(.cancelAction)
                    Button("Update") { model.startUpdate() }
                        .keyboardShortcut(.defaultAction)
                case .progress:
                    Button("Cancel") { model.cancelUpdate() }
                        .keyboardShortcut(.cancelAction)
                        .disabled(!model.updateCanCancel)
                case .message(_, _, let releasePage):
                    if releasePage {
                        Button("Open Release Page") { model.openReleasePage() }
                    }
                    Button("OK") { model.closeUpdateMessage() }
                        .keyboardShortcut(.defaultAction)
                case .none:
                    EmptyView()
                }
            }
        }
        .padding(20)
        .frame(width: 440)
        .interactiveDismissDisabled()
        .preferredColorScheme(.dark)
    }
}
