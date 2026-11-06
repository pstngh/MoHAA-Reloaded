// The updater's checks: what's offered, what's unpacked, and the swap with a failure at each of its steps.
import XCTest
@testable import LauncherCore

private let commitA = "aed952ba92532bf8596dc880a9afaf8fbfa4487b"
private let assetName = "openmohaa-metal-1.0.0-macos-arm64.zip"
private let hashA = String(repeating: "ab", count: 32)

private func releaseJSON(digest: String? = "sha256:\(hashA)", row: String? = hashA, state: String = "uploaded",
                         name: String = assetName, size: Int = 4_000_000,
                         url: String = "https://github.com/fortemoh/MoHAA-Reloaded/releases/download/v1.0.0/\(assetName)") -> Data {
    var asset: [String: Any] = ["name": name, "size": size, "state": state, "browser_download_url": url]
    if let digest { asset["digest"] = digest }
    let other: [String: Any] = ["name": "openmohaa-vulkan-1.0.0-windows-x64.zip", "size": 9, "digest": "sha256:00",
                                "state": "uploaded", "browser_download_url": "https://github.com/x"]
    var body = "| File | SHA-256 |\n|---|---|\n| `openmohaa-vulkan-1.0.0-windows-x64.zip` | `\(String(repeating: "0", count: 64))` |\n"
    if let row { body += "| `\(name)` | `\(row)` |\n" }
    return try! JSONSerialization.data(withJSONObject: ["body": body, "assets": [other, asset]])
}

private func json(_ object: Any) -> Data { try! JSONSerialization.data(withJSONObject: object) }

/// GitHub as the tests want it: by the path's end. A missing path is a 404.
private func fakeGitHub(release: Data = releaseJSON(), compare: String = "ahead", tagObject: String = "tag",
                        tagCommit: String = commitA, failing: String? = nil, status: Int = 200) -> UpdateFetch {
    let tagSha = "01ea1cc15be8513b0eb29c1f3be44f5a43efa90e"
    return { url in
        let path = url.path
        if let failing, path.contains(failing) {
            if status == 200 { throw URLError(.notConnectedToInternet) }
            return (status, Data("{\"message\":\"API rate limit exceeded\"}".utf8))
        }
        if path.hasSuffix("/releases/tags/v1.0.0") { return (200, release) }
        if path.hasSuffix("/git/ref/tags/v1.0.0") {
            let object: [String: Any] = ["sha": tagObject == "tag" ? tagSha : tagCommit, "type": tagObject]
            return (200, json(["object": object]))
        }
        if path.hasSuffix("/git/tags/\(tagSha)") {
            return (200, json(["object": ["sha": tagCommit, "type": "commit"]]))
        }
        if path.contains("/compare/") { return (200, json(["status": compare])) }
        return (404, Data())
    }
}

final class UpdateCheckTests: XCTestCase {
    func testReadsTheVersion() {
        func commit(_ text: String) -> String? { buildCommit(Data(text.utf8)) }
        XCTAssertEqual(commit("\0x+1.abcdef0\0OpenMoHAA 0.83.0-unstable+0.aed952b macosx-arm64 Nov 06 2026 UTC\0"), "aed952b")
        XCTAssertEqual(commit("\u{7f}0.83.0-unstable+0.cdce4088 client (%s)\0"), "cdce4088")
        XCTAssertEqual(commit("\u{7f}1.2.3+5.\(commitA)\0"), commitA)
        // no git, too short, run on, no version before it
        XCTAssertNil(commit("\u{7f}0.83.0-unstable+0\0"))
        XCTAssertNil(commit("\u{7f}0.83.0-unstable+0.aed952\0"))
        XCTAssertNil(commit("\u{7f}0.83.0-unstable+0.aed952bx\0"))
        XCTAssertNil(commit("\0unstable+0.aed952b\0"))
        XCTAssertNil(commit("\u{7f}83.0-unstable+0.aed952b\0"))
        XCTAssertNil(commit("+0.aed952b"))
        XCTAssertNil(commit(""))
    }

    func testReadsTheNotes() {
        let row = "| `\(assetName)` | `\(hashA)` |"
        XCTAssertEqual(notesSHA256("text\n\(row)\nmore", asset: assetName), hashA)
        XCTAssertEqual(notesSHA256("| `\(assetName)` | `\(hashA.uppercased())` |", asset: assetName), hashA)
        XCTAssertEqual(notesSHA256("\(row)\r\nx", asset: assetName), hashA)
        XCTAssertNil(notesSHA256("| `\(assetName)` | macOS 26 or later |", asset: assetName), "the download table's row")
        XCTAssertNil(notesSHA256("| `\(assetName)` | `abcd` |", asset: assetName))
        XCTAssertNil(notesSHA256("\(row)\n\(row)", asset: assetName), "two rows are no answer")
        XCTAssertNil(notesSHA256("| `other.zip` | `\(hashA)` |", asset: assetName))
        XCTAssertNil(notesSHA256("", asset: assetName))
    }

    /// The live notes name the download twice: in the table of downloads, then with its hash
    func testTheDownloadTableRowDoesntHideTheHashRow() {
        let notes = "| `\(assetName)` | macOS 26 or later on Apple Silicon |\n\n| `\(assetName)` | `\(hashA)` |\n"
        XCTAssertEqual(notesSHA256(notes, asset: assetName), hashA)
    }

    func testOffersOnlyWhenStrictlyAhead() async {
        let older = "cdce4088"
        let offer = await checkForUpdate(installed: older, fetch: fakeGitHub())
        XCTAssertEqual(offer?.commit, commitA)
        XCTAssertEqual(offer?.size, 4_000_000)
        XCTAssertEqual(offer?.sha256, hashA)
        // a lightweight tag works as well
        let plain = await checkForUpdate(installed: older, fetch: fakeGitHub(tagObject: "commit"))
        XCTAssertEqual(plain?.commit, commitA)
        for status in ["behind", "diverged", "identical", "", "AHEAD"] {
            let none = await checkForUpdate(installed: older, fetch: fakeGitHub(compare: status))
            XCTAssertNil(none, status)
        }
    }

    func testOffersNothingOtherwise() async {
        let older = "cdce4088"
        func none(_ installed: String?, _ fetch: UpdateFetch, _ why: String, line: UInt = #line) async {
            let offer = await checkForUpdate(installed: installed, fetch: fetch)
            XCTAssertNil(offer, why, line: line)
        }
        await none(String(commitA.prefix(7)), fakeGitHub(), "the same commit")
        await none(commitA, fakeGitHub(), "the same commit, in full")
        await none(nil, fakeGitHub(), "no commit in the binary")
        await none("xyz", fakeGitHub(), "not a commit")
        await none(older, fakeGitHub(failing: "/releases/"), "offline at the first call")
        await none(older, fakeGitHub(failing: "/git/ref/"), "offline at the second")
        await none(older, fakeGitHub(failing: "/compare/"), "offline at the last")
        await none(older, fakeGitHub(failing: "/releases/", status: 403), "the rate limit")
        await none(older, fakeGitHub(failing: "/compare/", status: 429), "the rate limit, later on")
        await none(older, fakeGitHub(failing: "/compare/", status: 404), "a commit GitHub doesn't know: a local build")
        await none(older, fakeGitHub(release: Data("<html>".utf8)), "an unexpected answer")
        await none(older, fakeGitHub(release: json(["assets": []])), "no download")
        await none(older, fakeGitHub(release: releaseJSON(state: "starter")), "still uploading")
        await none(older, fakeGitHub(release: releaseJSON(digest: nil)), "no digest")
        await none(older, fakeGitHub(release: releaseJSON(row: nil)), "no row in the notes")
        await none(older, fakeGitHub(release: releaseJSON(digest: "sha256:\(String(repeating: "cd", count: 32))")),
                   "CI between the upload and the notes: digest and row differ")
        await none(older, fakeGitHub(release: releaseJSON(url: "https://example.com/\(assetName)")), "not from GitHub")
        await none(older, fakeGitHub(release: releaseJSON(url: "http://github.com/\(assetName)")), "not https")
        await none(older, fakeGitHub(tagCommit: "short"), "a tag without a full commit")
    }

    func testZipPaths() {
        let top = "openmohaa-metal-1.0.0-macos-arm64"
        let good = ["\(top)/", "\(top)/COPYING.txt", "\(top)/openmohaa.app/", "\(top)/openmohaa.app/Contents/MacOS/openmohaa",
                    "\(top)/licenses/opus/COPYING"]
        XCTAssertTrue(zipPathsAreSafe(good, top: top))
        XCTAssertFalse(zipPathsAreSafe([], top: top))
        for bad in ["/etc/passwd", "../x", "\(top)/../x", "\(top)/a/../../x", "other/x", "x", "\(top)x/y", "\(top)//x",
                    "\(top)/./x", "\(top)\\..\\x", "\(top)/a\nb", "\(top)/._x", "\(top)/a/._b",
                    "\(top)/main/Pak0.pk3", "\(top)/MAIN/x", "\(top)/mohlauncher.json", "\(top)/.mohlauncher-update/x"] {
            XCTAssertFalse(zipPathsAreSafe(good + [bad], top: top), bad)
        }
    }

    func testFlagsStopBeforeTheFilesMoveOnly() {
        let moving = UpdateFlags()
        XCTAssertTrue(moving.beginInstall())
        XCTAssertFalse(moving.stop(), "moving: the window waits")
        let stopped = UpdateFlags()
        XCTAssertTrue(stopped.stop())
        XCTAssertFalse(stopped.beginInstall())
    }

    func testErrorsForThePlayer() {
        XCTAssertEqual(updateError(errno: ENOSPC), .diskFull)
        XCTAssertEqual(updateError(errno: EROFS), .unwritable)
        XCTAssertEqual(updateError(errno: EACCES), .unwritable)
        XCTAssertEqual(updateError(errno: EPERM), .blocked)
        XCTAssertEqual(updateError(errno: EIO), .failed)
        XCTAssertEqual(updateError(from: NSError(domain: NSCocoaErrorDomain, code: NSFileWriteOutOfSpaceError)), .diskFull)
        let wrapped = NSError(domain: NSCocoaErrorDomain, code: 1, userInfo: [NSUnderlyingErrorKey: NSError(domain: NSPOSIXErrorDomain, code: Int(EPERM))])
        XCTAssertEqual(updateError(from: wrapped), .blocked)
        for e in [UpdateError.cancelled, .tryLater, .network, .diskFull, .unwritable, .blocked, .gameRunning, .translocated, .interrupted, .failed] {
            XCTAssertFalse(e.message.contains("\n"))
            XCTAssertTrue(e.message.hasSuffix("."))
        }
    }
}

// MARK: Folders

private let fm = FileManager.default

private func tempFolder(_ name: String) throws -> URL {
    let url = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("mohl-\(name)-\(UUID().uuidString.prefix(8))")
    try fm.createDirectory(at: url, withIntermediateDirectories: true)
    return URL(fileURLWithPath: String(cString: realpath(url.path, nil)))
}

private func write(_ url: URL, _ text: String) throws {
    try fm.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try text.write(to: url, atomically: false, encoding: .utf8)
}

/// An app: what the updater looks at in one
private func makeApp(_ app: URL, id: String, exe: String, tag: String, binary: String? = nil, source: String? = nil) throws {
    var plist: [String: Any] = ["CFBundleIdentifier": id, "CFBundleExecutable": exe]
    if let source { plist[UpdateNames.sourceKey] = source }
    try fm.createDirectory(at: app.appendingPathComponent("Contents/MacOS"), withIntermediateDirectories: true)
    try PropertyListSerialization.data(fromPropertyList: plist, format: .xml, options: 0)
        .write(to: app.appendingPathComponent("Contents/Info.plist"))
    let file = app.appendingPathComponent("Contents/MacOS/\(exe)")
    try (binary ?? tag).write(to: file, atomically: false, encoding: .utf8)
    try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: file.path)
    try write(app.appendingPathComponent("Contents/Resources/tag"), tag)
}

private let gameID = "io.github.pstngh.openmohaa-metal", launcherID = "io.github.pstngh.mohlauncher"

private func makeGameFolder(_ folder: URL, tag: String = "old") throws {
    try makeApp(folder.appendingPathComponent(UpdateNames.game), id: gameID, exe: "openmohaa", tag: tag)
    try makeApp(folder.appendingPathComponent(UpdateNames.launcher), id: launcherID, exe: "mohlauncher", tag: tag)
    try write(folder.appendingPathComponent("INSTALL.txt"), "\(tag) install")
    try write(folder.appendingPathComponent("licenses/opus/COPYING"), "\(tag) opus")
    try write(folder.appendingPathComponent("main/Pak0.pk3"), "retail")
    try write(folder.appendingPathComponent("mohlauncher.json"), "{}")
}

/// A stage in the work folder, as unpackUpdate leaves it
private func makeStage(_ folder: URL, replaceLauncher: Bool = true) throws -> StagedUpdate {
    let stage = folder.appendingPathComponent("\(UpdateNames.workFolder)/new/top")
    try makeApp(stage.appendingPathComponent(UpdateNames.game), id: gameID, exe: "openmohaa", tag: "new")
    try makeApp(stage.appendingPathComponent(UpdateNames.launcher), id: launcherID, exe: "mohlauncher", tag: "new")
    let files = ["COPYING.txt", "INSTALL.txt", "licenses/opus/COPYING", "licenses/zlib/LICENSE"]
    for file in files { try write(stage.appendingPathComponent(file), "new \(file)") }
    return StagedUpdate(stage: stage, files: files, replaceLauncher: replaceLauncher, commit: commitA)
}

/// Every file below the folder and its contents, but the work folder
private func snapshot(_ folder: URL) throws -> [String: String] {
    var out: [String: String] = [:]
    let base = folder.path + "/"
    for case let url as URL in fm.enumerator(at: folder, includingPropertiesForKeys: nil)! {
        let inner = String(url.path.dropFirst(base.count))
        if inner.hasPrefix(UpdateNames.workFolder) { continue }
        var isDir: ObjCBool = false
        fm.fileExists(atPath: url.path, isDirectory: &isDir)
        out[inner] = isDir.boolValue ? "<folder>" : try String(contentsOf: url, encoding: .utf8)
    }
    return out
}

private func tag(_ folder: URL, _ app: String) -> String {
    (try? String(contentsOf: folder.appendingPathComponent("\(app)/Contents/Resources/tag"), encoding: .utf8)) ?? "none"
}

private func assertAllNew(_ folder: URL, launcherNew: Bool = true, _ why: String, line: UInt = #line) throws {
    XCTAssertEqual(tag(folder, UpdateNames.game), "new", why, line: line)
    XCTAssertEqual(tag(folder, UpdateNames.launcher), launcherNew ? "new" : "old", why, line: line)
    for file in ["COPYING.txt", "INSTALL.txt", "licenses/opus/COPYING", "licenses/zlib/LICENSE"] {
        XCTAssertEqual(try String(contentsOf: folder.appendingPathComponent(file), encoding: .utf8), "new \(file)", why, line: line)
    }
    XCTAssertEqual(try String(contentsOf: folder.appendingPathComponent("main/Pak0.pk3"), encoding: .utf8), "retail", why, line: line)
    XCTAssertEqual(try String(contentsOf: folder.appendingPathComponent("mohlauncher.json"), encoding: .utf8), "{}", why, line: line)
}

private struct InjectedError: Error {}

final class UpdateSwapTests: XCTestCase {
    /// The names of the steps of an update in this mode, as the failpoint hears them
    private func steps(forceRename: Bool, replaceLauncher: Bool = true) throws -> [String] {
        let folder = try tempFolder("steps")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let staged = try makeStage(folder, replaceLauncher: replaceLauncher)
        var names: [String] = []
        let installer = UpdateInstaller(folder: folder, failpoint: { names.append($0) }, forceRename: forceRename, verify: { _ in true })
        try installer.install(staged)
        try assertAllNew(folder, launcherNew: replaceLauncher, "the plain run")
        return names
    }

    /// One update with `failure` at the step named, from a fresh folder
    private func run(step: String, forceRename: Bool, replaceLauncher: Bool = true, failure: Error,
                     body: (URL, UpdateInstaller, Result<Void, Error>, [String: String]) throws -> Void) throws {
        let folder = try tempFolder("swap")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let before = try snapshot(folder)
        let staged = try makeStage(folder, replaceLauncher: replaceLauncher)
        var armed = true
        let installer = UpdateInstaller(folder: folder, failpoint: {
            if armed, $0 == step {
                armed = false
                throw failure
            }
        }, forceRename: forceRename, verify: { _ in true })
        let result = Result { try installer.install(staged) }
        XCTAssertFalse(armed, "\(step) was never reached")
        try body(folder, installer, result, before)
    }

    func testStepsAreWhatTheyShouldBe() throws {
        let swap = try steps(forceRename: false)
        XCTAssertEqual(Array(swap.prefix(3)), ["journal", "swap:\(UpdateNames.game)", "swap:\(UpdateNames.launcher)"])
        XCTAssertEqual(swap.last, "commit")
        XCTAssertTrue(swap.contains("file-aside:INSTALL.txt"))
        XCTAssertTrue(swap.contains("file-place:COPYING.txt"))
        XCTAssertFalse(swap.contains("file-aside:COPYING.txt"), "it wasn't there")
        let rename = try steps(forceRename: true)
        XCTAssertEqual(Array(rename.prefix(5)), ["journal", "aside:\(UpdateNames.game)", "place:\(UpdateNames.game)",
                                                 "aside:\(UpdateNames.launcher)", "place:\(UpdateNames.launcher)"])
    }

    /// The launcher's source is the same: no step touches it
    func testAnUnchangedLauncherIsLeftAlone() throws {
        for forceRename in [false, true] {
            let names = try steps(forceRename: forceRename, replaceLauncher: false)
            XCTAssertFalse(names.contains { $0.contains(UpdateNames.launcher) }, "\(names)")
        }
    }

    /// An error at any step: the folder as it was, the work folder gone, and nothing for the next start
    func testAnErrorAtEachStepLeavesTheFolderAsItWas() throws {
        for forceRename in [false, true] {
            for replaceLauncher in [true, false] {
                for step in try steps(forceRename: forceRename, replaceLauncher: replaceLauncher) {
                    let why = "\(step), rename \(forceRename), launcher \(replaceLauncher)"
                    try run(step: step, forceRename: forceRename, replaceLauncher: replaceLauncher, failure: InjectedError()) { folder, installer, result, before in
                        XCTAssertThrowsError(try result.get(), why)
                        XCTAssertEqual(try snapshot(folder), before, why)
                        // before the journal only the stage is there (runUpdate removes it, or the next start)
                        let outcome = try installer.recover()
                        XCTAssertTrue(outcome == .nothing || (step == "journal" && outcome == .cleaned), "\(why): \(outcome)")
                        XCTAssertFalse(fm.fileExists(atPath: installer.work.path), why)
                        XCTAssertEqual(try snapshot(folder), before, why)
                    }
                }
            }
        }
    }

    /// The process dies at any step: the next start leaves all of the old or all of the new, the pair matching, and
    /// no work folder
    func testACrashAtEachStepIsRecovered() throws {
        for forceRename in [false, true] {
            for step in try steps(forceRename: forceRename) {
                let why = "\(step), rename \(forceRename)"
                try run(step: step, forceRename: forceRename, failure: UpdateCrash()) { folder, installer, result, before in
                    XCTAssertThrowsError(try result.get(), why)
                    let outcome = try installer.recover()
                    switch outcome {
                    case .cleaned, .undone:
                        XCTAssertEqual(try snapshot(folder), before, why)
                    case .finished(let commit):
                        XCTAssertEqual(commit, commitA, why)
                        try assertAllNew(folder, why)
                    default:
                        XCTFail("\(why): \(outcome)")
                    }
                    XCTAssertFalse(fm.fileExists(atPath: installer.work.path), why)
                    XCTAssertEqual(try installer.recover(), .nothing, why)
                }
            }
        }
    }

    /// Dead between the game's swap and the launcher's: the game goes back. Both swapped: only small files were left,
    /// so it finishes.
    func testRecoveryUndoesAHalfPairAndFinishesAWholeOne() throws {
        try run(step: "swap:\(UpdateNames.launcher)", forceRename: false, failure: UpdateCrash()) { folder, installer, _, before in
            XCTAssertEqual(tag(folder, UpdateNames.game), "new")
            XCTAssertEqual(tag(folder, UpdateNames.launcher), "old")
            XCTAssertEqual(try installer.recover(), .undone(launcherReplaced: false))
            XCTAssertEqual(try snapshot(folder), before)
        }
        try run(step: "file-aside:INSTALL.txt", forceRename: false, failure: UpdateCrash()) { folder, installer, _, _ in
            XCTAssertEqual(tag(folder, UpdateNames.launcher), "new")
            XCTAssertEqual(try installer.recover(), .finished(commit: commitA))
            try assertAllNew(folder, "finished")
        }
    }

    /// A whole pair that doesn't verify is undone instead, and says the launcher changed under the one running
    func testRecoveryUndoesAPairThatDoesntVerify() throws {
        try run(step: "file-aside:INSTALL.txt", forceRename: false, failure: UpdateCrash()) { folder, _, _, before in
            let picky = UpdateInstaller(folder: folder, verify: { _ in false })
            XCTAssertEqual(try picky.recover(), .undone(launcherReplaced: true))
            XCTAssertEqual(try snapshot(folder), before)
        }
    }

    /// The process dies while it's undoing, at each undoing step: the next start still gets there
    func testACrashWhileUndoing() throws {
        for forceRename in [false, true] {
            // the error comes last, so everything is there to undo
            let undoing = try undoSteps(forceRename: forceRename)
            XCTAssertFalse(undoing.isEmpty)
            for step in undoing {
                let why = "\(step), rename \(forceRename)"
                let folder = try tempFolder("undo")
                defer { try? fm.removeItem(at: folder) }
                try makeGameFolder(folder)
                let before = try snapshot(folder)
                let staged = try makeStage(folder)
                let installer = UpdateInstaller(folder: folder, failpoint: {
                    if $0 == "commit" { throw InjectedError() }
                    if $0 == step { throw UpdateCrash() }
                }, forceRename: forceRename, verify: { _ in true })
                XCTAssertThrowsError(try installer.install(staged), why)
                let next = UpdateInstaller(folder: folder, verify: { _ in true })
                switch try next.recover() {
                case .finished: try assertAllNew(folder, why)
                case .undone, .cleaned: XCTAssertEqual(try snapshot(folder), before, why)
                case let other: XCTFail("\(why): \(other)")
                }
                XCTAssertFalse(fm.fileExists(atPath: next.work.path), why)
            }
        }
    }

    private func undoSteps(forceRename: Bool) throws -> [String] {
        let folder = try tempFolder("undosteps")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let staged = try makeStage(folder)
        var names: [String] = []
        let installer = UpdateInstaller(folder: folder, failpoint: {
            if $0 == "commit" { throw InjectedError() }
            if $0.hasPrefix("undo") { names.append($0) }
        }, forceRename: forceRename, verify: { _ in true })
        XCTAssertThrowsError(try installer.install(staged))
        return names
    }

    /// The undoing itself fails: the error says so, the journal stays, and the next start finishes the job
    func testAFailedUndoingIsLeftForTheNextStart() throws {
        for forceRename in [false, true] {
            let undoing = try undoSteps(forceRename: forceRename)
            for step in undoing where step.contains(".app") {
                let why = "\(step), rename \(forceRename)"
                let folder = try tempFolder("undofail")
                defer { try? fm.removeItem(at: folder) }
                try makeGameFolder(folder)
                let before = try snapshot(folder)
                let staged = try makeStage(folder)
                var armed = true
                let installer = UpdateInstaller(folder: folder, failpoint: {
                    if $0 == "commit" { throw InjectedError() }
                    if armed, $0 == step {
                        armed = false
                        throw InjectedError()
                    }
                }, forceRename: forceRename, verify: { _ in true })
                XCTAssertThrowsError(try installer.install(staged), why) { XCTAssertEqual($0 as? UpdateError, .interrupted, why) }
                XCTAssertTrue(fm.fileExists(atPath: installer.work.path), why)
                let next = UpdateInstaller(folder: folder, verify: { _ in false }) // so that it undoes
                XCTAssertEqual(try next.recover(), .undone(launcherReplaced: step.contains(UpdateNames.launcher)), why)
                XCTAssertEqual(try snapshot(folder), before, why)
            }
        }
    }

    func testRecoveryLeavesARunningGameAlone() throws {
        try run(step: "swap:\(UpdateNames.launcher)", forceRename: false, failure: UpdateCrash()) { folder, installer, _, before in
            let half = try snapshot(folder)
            XCTAssertEqual(try installer.recover(gameRunning: { true }), .gameRunning)
            XCTAssertEqual(try snapshot(folder), half)
            XCTAssertEqual(try installer.recover(), .undone(launcherReplaced: false))
            XCTAssertEqual(try snapshot(folder), before)
        }
    }

    /// Something else took the app's place: nothing the journal knows, so nothing is touched
    func testRecoveryWontGuess() throws {
        try run(step: "swap:\(UpdateNames.launcher)", forceRename: false, failure: UpdateCrash()) { folder, installer, _, _ in
            let game = folder.appendingPathComponent(UpdateNames.game)
            try fm.removeItem(at: game)
            try makeApp(game, id: gameID, exe: "openmohaa", tag: "someone else's")
            let there = try snapshot(folder)
            XCTAssertEqual(try installer.recover(), .stuck)
            XCTAssertEqual(try snapshot(folder), there)
            XCTAssertTrue(fm.fileExists(atPath: installer.work.path))
        }
    }

    func testCleansWhatAnEarlyStopLeft() throws {
        let folder = try tempFolder("early")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let before = try snapshot(folder)
        let installer = UpdateInstaller(folder: folder)
        XCTAssertEqual(try installer.recover(), .nothing)
        try write(installer.work.appendingPathComponent("half.zip"), "half")
        XCTAssertEqual(try installer.recover(), .cleaned)
        XCTAssertFalse(fm.fileExists(atPath: installer.work.path))
        // a journal that isn't one is left alone
        try write(installer.work.appendingPathComponent("journal.json"), "not json")
        XCTAssertEqual(try installer.recover(), .stuck)
        try fm.removeItem(at: installer.work)
        XCTAssertEqual(try snapshot(folder), before)
    }

    /// A folder where the download has a file, and a file where it needs a folder: nothing moves
    func testNeverInPlaceOfAFolder() throws {
        let folder = try tempFolder("infolder")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        try fm.removeItem(at: folder.appendingPathComponent("INSTALL.txt"))
        try write(folder.appendingPathComponent("INSTALL.txt/inside"), "keep")
        let before = try snapshot(folder)
        let staged = try makeStage(folder)
        XCTAssertThrowsError(try UpdateInstaller(folder: folder, verify: { _ in true }).install(staged))
        XCTAssertEqual(try snapshot(folder), before)
    }

    /// The other half of the same-source rule: only a stamp on both sides says so
    func testSameLauncherSource() throws {
        let folder = try tempFolder("source")
        defer { try? fm.removeItem(at: folder) }
        let a = folder.appendingPathComponent("a.app"), b = folder.appendingPathComponent("b.app")
        try makeApp(a, id: launcherID, exe: "mohlauncher", tag: "a", source: "tree1")
        try makeApp(b, id: launcherID, exe: "mohlauncher", tag: "b", source: "tree1")
        XCTAssertTrue(sameLauncherSource(a, b))
        try makeApp(b, id: launcherID, exe: "mohlauncher", tag: "b", source: "tree2")
        XCTAssertFalse(sameLauncherSource(a, b))
        try makeApp(b, id: launcherID, exe: "mohlauncher", tag: "b")
        XCTAssertFalse(sameLauncherSource(a, b), "today's launcher has no stamp")
        XCTAssertFalse(sameLauncherSource(b, a))
        try makeApp(a, id: launcherID, exe: "mohlauncher", tag: "a")
        XCTAssertFalse(sameLauncherSource(a, b), "two without stamps say nothing")
    }

    func testLockIsOnePerFolder() throws {
        let folder = try tempFolder("lock")
        defer { try? fm.removeItem(at: folder) }
        let first = UpdateLock(folder: folder)
        XCTAssertNotNil(first)
        XCTAssertNil(UpdateLock(folder: folder))
        first?.release()
        XCTAssertNotNil(UpdateLock(folder: folder))
    }

    func testOnlyTheLauncherBesideTheGameUpdates() throws {
        let folder = try tempFolder("layout")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let launcher = folder.appendingPathComponent(UpdateNames.launcher)
        XCTAssertEqual(updateFolder(launcherBundle: launcher)?.path, folder.path)
        XCTAssertNil(updateFolder(launcherBundle: folder.appendingPathComponent("Renamed.app")))
        try fm.removeItem(at: folder.appendingPathComponent(UpdateNames.game))
        XCTAssertNil(updateFolder(launcherBundle: launcher), "no game beside it")
    }

    func testFindsARunningGame() throws {
        let folder = try tempFolder("running")
        defer { try? fm.removeItem(at: folder) }
        let macos = folder.appendingPathComponent("\(UpdateNames.game)/Contents/MacOS")
        try fm.createDirectory(at: macos, withIntermediateDirectories: true)
        let exe = macos.appendingPathComponent("openmohaa")
        try fm.copyItem(atPath: "/bin/sleep", toPath: exe.path)
        XCTAssertFalse(gameIsRunning(folder: folder))
        let process = Process()
        process.executableURL = exe
        process.arguments = ["30"]
        try process.run()
        defer { process.terminate() }
        XCTAssertTrue(gameIsRunning(folder: folder))
        process.terminate()
        process.waitUntilExit()
        XCTAssertFalse(gameIsRunning(folder: folder))
    }

    func testClearsQuarantineAndChecks() throws {
        let folder = try tempFolder("quarantine")
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        let exe = folder.appendingPathComponent("\(UpdateNames.game)/Contents/MacOS/openmohaa")
        let pak = folder.appendingPathComponent("main/Pak0.pk3")
        for url in [exe, pak] {
            XCTAssertEqual(setxattr(url.path, "com.apple.quarantine", "0083;00000000;Safari;", 21, 0, 0), 0)
        }
        XCTAssertEqual(try quarantinedPaths(installedItems(folder: folder)).map(\.path), [exe.path])
        try clearQuarantine(installedItems(folder: folder))
        XCTAssertTrue(try quarantinedPaths(installedItems(folder: folder)).isEmpty)
        XCTAssertEqual(try quarantinedPaths([pak]).map(\.path), [pak.path], "the game's data isn't the launcher's to touch")
    }

    /// The trap that decides the design: on FAT32 RENAME_SWAP returns 0 and overwrites, and the volume says it hasn't
    /// got it. A real FAT32 and exFAT image, with the rename path doing the update on each.
    func testSwapOnlyWhereTheVolumeSaysSo() throws {
        let folder = try tempFolder("volumes")
        defer { try? fm.removeItem(at: folder) }
        XCTAssertTrue(volumeSupportsSwap(folder.path) || !isAPFS(folder), "APFS has it")
        for (fs, name) in [("MS-DOS FAT32", "S83TFAT"), ("ExFAT", "S83TEX")] {
            let image = folder.appendingPathComponent("\(name).dmg")
            let made = runTool("/usr/bin/hdiutil", ["create", "-size", "64m", "-fs", fs, "-volname", name, image.path])
            let mount = URL(fileURLWithPath: "/Volumes/\(name)")
            if fm.fileExists(atPath: mount.path) { _ = runTool("/usr/bin/hdiutil", ["detach", "-force", mount.path]) }
            guard made.status == 0 else { throw XCTSkip("no disk images here: \(made.output)") }
            let attached = runTool("/usr/bin/hdiutil", ["attach", "-nobrowse", image.path])
            guard attached.status == 0 else { throw XCTSkip("no disk images here: \(attached.output)") }
            defer { _ = runTool("/usr/bin/hdiutil", ["detach", "-force", mount.path]) }
            XCTAssertFalse(volumeSupportsSwap(mount.path), fs)
            let game = mount.appendingPathComponent("g")
            try makeGameFolder(game)
            let staged = try makeStage(game)
            let installer = UpdateInstaller(folder: game, verify: { _ in true })
            try installer.install(staged)
            try installer.finishInstall(replacedLauncher: false)
            try assertAllNew(game, fs)
            XCTAssertFalse(fm.fileExists(atPath: installer.work.path), fs)
        }
    }

    private func isAPFS(_ url: URL) -> Bool {
        var fs = statfs()
        guard statfs(url.path, &fs) == 0 else { return false }
        return withUnsafePointer(to: &fs.f_fstypename) { $0.withMemoryRebound(to: CChar.self, capacity: 16) { String(cString: $0) } } == "apfs"
    }
}

// MARK: The whole update, with a stubbed network

private final class StubProtocol: URLProtocol {
    nonisolated(unsafe) static var status = 200
    nonisolated(unsafe) static var body = Data()
    /// After this many bytes the connection breaks
    nonisolated(unsafe) static var breakAfter: Int?
    /// Called once the first data went out: a test's Cancel
    nonisolated(unsafe) static var afterFirstChunk: (() -> Void)?
    nonisolated(unsafe) static var holdAfterFirstChunk = false

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func stopLoading() {}

    override func startLoading() {
        let response = HTTPURLResponse(url: request.url!, statusCode: Self.status, httpVersion: "HTTP/1.1",
                                       headerFields: ["Content-Length": "\(Self.body.count)"])!
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        let data = Self.breakAfter.map { Self.body.prefix($0) } ?? Self.body[...]
        if Self.holdAfterFirstChunk {
            client?.urlProtocol(self, didLoad: Data(data.prefix(1000)))
            Self.afterFirstChunk?()
            return // the connection hangs: only Cancel ends it
        }
        client?.urlProtocol(self, didLoad: Data(data))
        if Self.breakAfter != nil {
            client?.urlProtocol(self, didFailWithError: URLError(.networkConnectionLost))
        } else {
            client?.urlProtocolDidFinishLoading(self)
        }
    }
}

final class UpdateRunTests: XCTestCase {
    private var folder: URL!
    private var zip: Data!
    private var offer: UpdateOffer!
    private let top = String(assetName.dropLast(4))
    private let built = "aed952b"

    private var configuration: URLSessionConfiguration {
        let c = URLSessionConfiguration.ephemeral
        c.protocolClasses = [StubProtocol.self]
        return c
    }

    override func setUpWithError() throws {
        folder = try tempFolder("run")
        try makeGameFolder(folder)
        let source = try tempFolder("zip")
        defer { try? fm.removeItem(at: source) }
        let inner = source.appendingPathComponent(top)
        try makeZipTree(inner)
        let out = source.appendingPathComponent("a.zip")
        XCTAssertEqual(runTool("/usr/bin/ditto", ["-c", "-k", "--norsrc", "--keepParent", inner.path, out.path]).status, 0)
        zip = try Data(contentsOf: out)
        offer = UpdateOffer(commit: commitA, asset: assetName, url: URL(string: "https://github.com/x/\(assetName)")!,
                            size: Int64(zip.count), sha256: try fileSHA256(out))
        StubProtocol.status = 200
        StubProtocol.body = zip
        StubProtocol.breakAfter = nil
        StubProtocol.afterFirstChunk = nil
        StubProtocol.holdAfterFirstChunk = false
    }

    override func tearDownWithError() throws {
        try? fm.removeItem(at: folder)
    }

    private func makeZipTree(_ inner: URL, launcherSource: String? = nil) throws {
        try makeApp(inner.appendingPathComponent(UpdateNames.game), id: gameID, exe: "openmohaa", tag: "new",
                    binary: "\u{7f}OpenMoHAA 0.83.0-unstable+0.\(built) macosx-arm64\0")
        try makeApp(inner.appendingPathComponent(UpdateNames.launcher), id: launcherID, exe: "mohlauncher", tag: "new", source: launcherSource)
        for file in ["COPYING.txt", "INSTALL.txt", "licenses/opus/COPYING", "licenses/zlib/LICENSE"] {
            try write(inner.appendingPathComponent(file), "new \(file)")
        }
    }

    private func update(_ flags: UpdateFlags = UpdateFlags(), hooks: UpdateHooks = UpdateHooks(), verify: @escaping (URL) -> Bool = { _ in true }) async -> Result<Bool, Error> {
        do {
            return .success(try await runUpdate(offer, folder: folder, flags: flags, hooks: hooks, configuration: configuration, verify: verify) { _ in })
        } catch {
            return .failure(error)
        }
    }

    private func assertFailed(_ result: Result<Bool, Error>, _ expected: UpdateError, _ before: [String: String], line: UInt = #line) throws {
        guard case .failure(let error) = result else { return XCTFail("it succeeded", line: line) }
        XCTAssertEqual(error as? UpdateError, expected, line: line)
        XCTAssertEqual(try snapshot(folder), before, line: line)
        XCTAssertFalse(fm.fileExists(atPath: folder.appendingPathComponent(UpdateNames.workFolder).path), line: line)
    }

    func testAWholeUpdate() async throws {
        let result = await update()
        XCTAssertEqual(try result.get(), true)
        try assertAllNew(folder, "done")
        // the running launcher's old bundle waits in the work folder for the next start
        let installer = UpdateInstaller(folder: folder)
        XCTAssertEqual(try installer.recover(), .finished(commit: commitA))
        XCTAssertFalse(fm.fileExists(atPath: installer.work.path))
    }

    func testTheSameSourceLeavesTheLauncher() async throws {
        let launcher = folder.appendingPathComponent(UpdateNames.launcher)
        try makeApp(launcher, id: launcherID, exe: "mohlauncher", tag: "old", source: "tree1")
        let source = try tempFolder("zip2")
        defer { try? fm.removeItem(at: source) }
        let inner = source.appendingPathComponent(top)
        try makeZipTree(inner, launcherSource: "tree1")
        let out = source.appendingPathComponent("a.zip")
        XCTAssertEqual(runTool("/usr/bin/ditto", ["-c", "-k", "--norsrc", "--keepParent", inner.path, out.path]).status, 0)
        StubProtocol.body = try Data(contentsOf: out)
        offer.size = Int64(StubProtocol.body.count)
        offer.sha256 = try fileSHA256(out)
        let result = await update()
        XCTAssertEqual(try result.get(), false, "no restart")
        try assertAllNew(folder, launcherNew: false, "the game only")
        XCTAssertFalse(fm.fileExists(atPath: folder.appendingPathComponent(UpdateNames.workFolder).path), "done at once")
    }

    func testTheNetworkFails() async throws {
        let before = try snapshot(folder)
        StubProtocol.breakAfter = 100
        try assertFailed(await update(), .network, before)
    }

    func testCIWindows() async throws {
        let before = try snapshot(folder)
        StubProtocol.status = 404
        try assertFailed(await update(), .tryLater, before)
        StubProtocol.status = 200
        // a shorter or longer file than the release says
        StubProtocol.body = zip.dropLast(10)
        try assertFailed(await update(), .tryLater, before)
        StubProtocol.body = zip + Data(repeating: 0, count: 10)
        try assertFailed(await update(), .tryLater, before)
        // the size is right and the hash isn't
        var other = zip!
        other[other.count / 2] ^= 0xFF
        StubProtocol.body = other
        try assertFailed(await update(), .tryLater, before)
    }

    /// The local test hook's corruption: after the download, before the check
    func testACorruptedFile() async throws {
        let before = try snapshot(folder)
        var hooks = UpdateHooks()
        hooks.afterDownload = { url in
            let handle = try! FileHandle(forUpdating: url)
            try! handle.seek(toOffset: 100)
            try! handle.write(contentsOf: Data([0xFF, 0x00, 0xFF]))
            try! handle.close()
        }
        try assertFailed(await update(hooks: hooks), .tryLater, before)
    }

    func testCancelDuringTheDownload() async throws {
        let before = try snapshot(folder)
        let flags = UpdateFlags()
        StubProtocol.holdAfterFirstChunk = true
        StubProtocol.afterFirstChunk = { flags.stop() }
        try assertFailed(await update(flags), .cancelled, before)
    }

    func testCancelBeforeTheFilesMove() async throws {
        let before = try snapshot(folder)
        let flags = UpdateFlags()
        var hooks = UpdateHooks()
        hooks.afterDownload = { _ in flags.stop() }
        try assertFailed(await update(flags, hooks: hooks), .cancelled, before)
    }

    func testABadSignatureAtTheCheck() async throws {
        let before = try snapshot(folder)
        try assertFailed(await update(verify: { $0.lastPathComponent != UpdateNames.launcher }), .tryLater, before)
    }

    func testOtherBundleIDs() async throws {
        let before = try snapshot(folder)
        try makeApp(folder.appendingPathComponent(UpdateNames.game), id: "some.other.game", exe: "openmohaa", tag: "old")
        let changed = try snapshot(folder)
        XCTAssertNotEqual(before, changed)
        try assertFailed(await update(), .tryLater, changed)
    }

    func testAZipOfAnotherCommit() async throws {
        let before = try snapshot(folder)
        offer.commit = "cdce4088" + String(repeating: "0", count: 32)
        try assertFailed(await update(), .tryLater, before)
    }

    func testAZipWithoutBothApps() async throws {
        let before = try snapshot(folder)
        try replaceZip { try fm.removeItem(at: $0.appendingPathComponent(UpdateNames.launcher)) }
        try assertFailed(await update(), .tryLater, before)
    }

    func testAZipWithALink() async throws {
        let before = try snapshot(folder)
        try replaceZip { try fm.createSymbolicLink(atPath: $0.appendingPathComponent("link").path, withDestinationPath: "/etc") }
        try assertFailed(await update(), .tryLater, before)
    }

    func testAZipThatLeavesItsFolder() async throws {
        let before = try snapshot(folder)
        try replaceZip { try fm.createDirectory(at: $0.appendingPathComponent("main"), withIntermediateDirectories: true)
            try write($0.appendingPathComponent("main/x"), "over the player's data") }
        try assertFailed(await update(), .tryLater, before)
    }

    private func replaceZip(_ change: (URL) throws -> Void) throws {
        let source = try tempFolder("zip3")
        defer { try? fm.removeItem(at: source) }
        let inner = source.appendingPathComponent(top)
        try makeZipTree(inner)
        try change(inner)
        let out = source.appendingPathComponent("a.zip")
        XCTAssertEqual(runTool("/usr/bin/ditto", ["-c", "-k", "--norsrc", "--keepParent", inner.path, out.path]).status, 0)
        StubProtocol.body = try Data(contentsOf: out)
        offer.size = Int64(StubProtocol.body.count)
        offer.sha256 = try fileSHA256(out)
    }

    func testAFolderThatCantBeWritten() async throws {
        let before = try snapshot(folder)
        try fm.setAttributes([.posixPermissions: 0o555], ofItemAtPath: folder.path)
        defer { try? fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: folder.path) }
        guard !fm.isWritableFile(atPath: folder.path) else { throw XCTSkip("this volume ignores permissions") }
        try assertFailed(await update(), .unwritable, before)
    }

    func testARunningGameStopsIt() async throws {
        let exe = folder.appendingPathComponent("\(UpdateNames.game)/Contents/MacOS/openmohaa")
        try fm.removeItem(at: exe)
        try fm.copyItem(atPath: "/bin/sleep", toPath: exe.path)
        let process = Process()
        process.executableURL = exe
        process.arguments = ["30"]
        try process.run()
        defer { process.terminate() }
        let before = try binarySafeSnapshot()
        let result = await update()
        guard case .failure(let error) = result else { return XCTFail("it succeeded") }
        XCTAssertEqual(error as? UpdateError, .gameRunning)
        XCTAssertEqual(try binarySafeSnapshot(), before)
    }

    private func binarySafeSnapshot() throws -> [String] {
        try fm.subpathsOfDirectory(atPath: folder.path).sorted()
    }
}

/// The whole update against the live release, in a folder whose apps are older: MOHL_LIVE_TEST=<a folder to work in>
/// (`swift test --filter UpdateLiveTests`). It downloads the real zip and checks its real signatures.
final class UpdateLiveTests: XCTestCase {
    func testUpdatesFromTheRelease() async throws {
        guard let base = ProcessInfo.processInfo.environment["MOHL_LIVE_TEST"] else {
            throw XCTSkip("MOHL_LIVE_TEST isn't set")
        }
        let folder = URL(fileURLWithPath: base).appendingPathComponent("mohl-live-\(UUID().uuidString.prefix(8))")
        try fm.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? fm.removeItem(at: folder) }
        try makeGameFolder(folder)
        // d4f7c7c8: an ancestor of what v1.0.0 has been on since 2026-11-06
        try makeApp(folder.appendingPathComponent(UpdateNames.game), id: gameID, exe: "openmohaa", tag: "old",
                    binary: "\u{7f}OpenMoHAA 0.83.0-unstable+0.d4f7c7c macosx-arm64 Nov 06 2026 UTC\0")
        let installed = installedCommit(folder: folder)
        XCTAssertEqual(installed, "d4f7c7c")
        let found = await checkForUpdate(installed: installed, fetch: { try await githubFetch($0) })
        let offer = try XCTUnwrap(found, "the release is newer than d4f7c7c")
        XCTAssertEqual(offer.commit.count, 40)
        XCTAssertGreaterThan(offer.size, 1_000_000)

        var seen = false
        let replaced = try await runUpdate(offer, folder: folder, flags: UpdateFlags()) { if case .install = $0 { seen = true } }
        XCTAssertTrue(seen)
        XCTAssertTrue(replaced, "today's installed launcher has no stamp")
        let now = try XCTUnwrap(installedCommit(folder: folder))
        XCTAssertTrue(offer.commit.hasPrefix(now))
        XCTAssertTrue(codesignVerifies(folder.appendingPathComponent(UpdateNames.game)))
        XCTAssertTrue(codesignVerifies(folder.appendingPathComponent(UpdateNames.launcher)))
        XCTAssertEqual(try quarantinedPaths(installedItems(folder: folder)), [])
        XCTAssertEqual(try String(contentsOf: folder.appendingPathComponent("main/Pak0.pk3"), encoding: .utf8), "retail")
        XCTAssertEqual(try String(contentsOf: folder.appendingPathComponent("mohlauncher.json"), encoding: .utf8), "{}")
        XCTAssertTrue(fm.fileExists(atPath: folder.appendingPathComponent("INSTALL.txt").path))
        let recovered = try UpdateInstaller(folder: folder).recover()
        XCTAssertEqual(recovered, .finished(commit: offer.commit))
        XCTAssertFalse(fm.fileExists(atPath: folder.appendingPathComponent(UpdateNames.workFolder).path))
        let again = await checkForUpdate(installed: now, fetch: { try await githubFetch($0) })
        XCTAssertNil(again, "up to date now")
    }
}
