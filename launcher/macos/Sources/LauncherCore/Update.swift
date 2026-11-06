// Updates, as launcher/desktop/src/update.rs has them on `vulkan`: whether the release has a newer build than the
// game beside the launcher, and that build's files in place of the old ones. CI replaces the Mac download in v1.0.0
// with each code push to `metal`, in this order: the zip, the tag, the notes' SHA-256 (macos-metal.yml, release-v1).
// So a build is told by its commit, and a zip whose digest and notes' row differ is CI at work: try later.
//
// Nothing moves until the download is checked, and what moves is journaled: at any point a start of the launcher can
// finish the update or undo it (UpdateInstaller.recover). What step 1 of the plan's §3.1 #65 measured on macOS 26:
// - RENAME_SWAP is only safe on a volume that says it has it: FAT32 ignores the flag, returns 0 and overwrites.
// - App Management doesn't stop these renames, and each new ad-hoc build is asked for folder access again.
import CryptoKit
import Darwin
import Foundation

public enum UpdateNames {
    public static let repository = "fortemoh/MoHAA-Reloaded"
    public static let tag = "v1.0.0"
    public static let assetPrefix = "openmohaa-metal-"
    public static let assetSuffix = "-macos-arm64.zip"
    /// Where an update downloads and unpacks, in the game folder: the same volume, so files move by being renamed
    public static let workFolder = ".mohlauncher-update"
    public static let game = "openmohaa.app"
    public static let launcher = "mohlauncher.app"
    /// Where the old launcher waits on a volume without RENAME_SWAP: beside the game, under a name that can be opened.
    /// There's a moment with no mohlauncher.app there, and a player must still have a launcher to recover with.
    public static let launcherAside = "mohlauncher-update-interrupted.app"
    /// The git tree of launcher/ a launcher was built from (bundle.sh): the same on both sides means nothing changed.
    /// CI's builds differ byte for byte even from the same source, so the bytes can't tell.
    public static let sourceKey = "MOHLauncherSource"
    public static let releasePage = URL(string: "https://github.com/\(repository)/releases/tag/\(tag)")!
    static let quarantine = "com.apple.quarantine"
    /// Never overwritten by a download's files: the player's data
    static let reserved: Set<String> = ["main", "mainta", "maintt", "mohlauncher.json", workFolder]
}

/// Why an update didn't happen. The folder is as it was, except after .interrupted (the next start tries again).
public enum UpdateError: Error, Equatable {
    case cancelled, tryLater, network, diskFull, unwritable, blocked, gameRunning, translocated, interrupted, failed, busy

    /// One plain sentence for the player
    public var message: String {
        switch self {
        case .cancelled: return "The update was cancelled."
        case .tryLater: return "The new version isn't ready on GitHub yet: try again later."
        case .network: return "The update couldn't be downloaded: check your connection and try again."
        case .diskFull: return "There isn't enough free disk space for the update."
        case .unwritable: return "MoHAA Launcher can't write to the game folder, so nothing was changed."
        case .blocked: return "macOS didn't allow MoHAA Launcher to replace the apps: allow it in System Settings, Privacy & Security, App Management, and try again."
        case .gameRunning: return "The game is running: quit it, then update."
        case .translocated: return "macOS is running MoHAA Launcher from a temporary copy, so it can't update itself: use the command in INSTALL.txt, or download the new version."
        case .interrupted: return "The update couldn't be finished or undone: open MoHAA Launcher again, or download the game again if this stays."
        case .failed: return "The update couldn't be installed, so nothing was changed: try again later."
        case .busy: return "Another MoHAA Launcher is working on this game folder: close it and try again."
        }
    }
}

/// A test's stand-in for the process dying: nothing after it runs, no rollback either
public struct UpdateCrash: Error {
    public init() {}
}

/// The error an operating system error means for the player
public func updateError(from error: Error) -> UpdateError {
    if let known = error as? UpdateError { return known }
    var current: NSError? = error as NSError
    while let e = current {
        if e.domain == NSPOSIXErrorDomain { return updateError(errno: Int32(e.code)) }
        if e.domain == NSCocoaErrorDomain {
            if e.code == NSFileWriteOutOfSpaceError { return .diskFull }
            if e.code == NSFileWriteNoPermissionError || e.code == NSFileWriteVolumeReadOnlyError { return .unwritable }
        }
        current = e.userInfo[NSUnderlyingErrorKey] as? NSError
    }
    return .failed
}

public func updateError(errno code: Int32) -> UpdateError {
    switch code {
    case ENOSPC, EDQUOT: return .diskFull
    case EACCES, EROFS: return .unwritable
    case EPERM: return .blocked
    default: return .failed
    }
}

private func posixError(_ code: Int32 = errno) -> NSError {
    NSError(domain: NSPOSIXErrorDomain, code: Int(code))
}

// MARK: The check

/// The commit in a build's version, as the game's program holds it: "0.83.0-unstable+0.aed952b" is major, minor and
/// patch, the stage, then the CI run and the commit abbreviated (q_version.h's PRODUCT_VERSION_FULL). A build made
/// without git's information has none.
public func buildCommit(_ binary: Data) -> String? {
    let bytes = [UInt8](binary)
    let plus = UInt8(ascii: "+"), dot = UInt8(ascii: ".")
    func digit(_ c: UInt8) -> Bool { c >= 48 && c <= 57 }
    func hex(_ c: UInt8) -> Bool { digit(c) || (c >= 97 && c <= 102) }
    func alnum(_ c: UInt8) -> Bool { digit(c) || (c >= 97 && c <= 122) || (c >= 65 && c <= 90) }
    var at = 0
    while at < bytes.count {
        defer { at += 1 }
        guard bytes[at] == plus, endsWithVersion(bytes, at) else { continue }
        var k = at + 1
        let runStart = k
        while k < bytes.count, digit(bytes[k]) { k += 1 }
        guard k > runStart, k < bytes.count, bytes[k] == dot else { continue }
        k += 1
        let start = k
        while k < bytes.count, hex(bytes[k]) { k += 1 }
        let count = k - start
        guard (7...40).contains(count), k == bytes.count || !alnum(bytes[k]) else { continue }
        return String(decoding: bytes[start..<k], as: UTF8.self)
    }
    return nil
}

/// "0.83.0" or "0.83.0-unstable" just before `end`
private func endsWithVersion(_ b: [UInt8], _ end: Int) -> Bool {
    var k = end
    func letter(_ c: UInt8) -> Bool { (c >= 97 && c <= 122) || (c >= 65 && c <= 90) }
    let stageEnd = k
    while k > 0, letter(b[k - 1]) { k -= 1 }
    if k < stageEnd {
        guard k > 0, b[k - 1] == UInt8(ascii: "-") else { return false }
        k -= 1
    }
    for part in 0..<3 {
        let digitsEnd = k
        while k > 0, b[k - 1] >= 48, b[k - 1] <= 57 { k -= 1 }
        guard k < digitsEnd else { return false }
        if part < 2 {
            guard k > 0, b[k - 1] == UInt8(ascii: ".") else { return false }
            k -= 1
        }
    }
    return true
}

/// A download's SHA-256 in the notes' table ("| `name` | `<64 hex digits>` |"), in lowercase. CI's job fails unless
/// there is exactly one such row: two here (or a short one) are no answer.
public func notesSHA256(_ notes: String, asset: String) -> String? {
    let cell = "| `\(asset)` | `"
    var found: [String] = []
    for line in notes.split(whereSeparator: \.isNewline) {
        guard let range = line.range(of: cell) else { continue }
        let hex = line[range.upperBound...].prefix { $0.isHexDigit }
        guard hex.count == 64, line[range.upperBound...].dropFirst(64).hasPrefix("`") else { return nil }
        found.append(hex.lowercased())
    }
    return found.count == 1 ? found[0] : nil
}

/// A newer build than the game's
public struct UpdateOffer: Equatable, Sendable {
    /// The tag's commit, in full
    public var commit: String
    public var asset: String
    public var url: URL
    public var size: Int64
    /// In lowercase hex; GitHub's digest and the notes' row agree on it
    public var sha256: String
    /// The game's commit this was found newer than: another game in the folder since makes it void
    public var installed: String

    public init(commit: String, asset: String, url: URL, size: Int64, sha256: String, installed: String = "") {
        (self.commit, self.asset, self.url, self.size, self.sha256, self.installed) = (commit, asset, url, size, sha256, installed)
    }

    public var shortCommit: String { String(commit.prefix(8)) }
}

private struct ReleaseJSON: Decodable {
    struct Asset: Decodable {
        var name: String
        var size: Int64
        var digest: String?
        var state: String?
        var browser_download_url: String
    }
    var body: String?
    var assets: [Asset]?
}

private struct RefJSON: Decodable {
    struct Object: Decodable {
        var sha: String
        var type: String
    }
    var object: Object
}

private struct CompareJSON: Decodable {
    var status: String
}

private func isFullCommit(_ s: String) -> Bool {
    s.count == 40 && s.allSatisfy { $0.isHexDigit && !$0.isUppercase }
}

/// The Mac download of a release's JSON, if it's complete: one asset, fully uploaded, from GitHub, with a digest that
/// equals the notes' row. Anything else is no download for now (the moment CI replaces it, too).
func releaseAsset(_ json: Data) -> (asset: String, url: URL, size: Int64, sha256: String)? {
    guard let release = try? JSONDecoder().decode(ReleaseJSON.self, from: json) else { return nil }
    let matching = (release.assets ?? []).filter {
        $0.name.hasPrefix(UpdateNames.assetPrefix) && $0.name.hasSuffix(UpdateNames.assetSuffix)
    }
    guard matching.count == 1, let asset = matching.first, asset.state == nil || asset.state == "uploaded",
          asset.size > 0, let digest = asset.digest, digest.hasPrefix("sha256:"),
          let url = URL(string: asset.browser_download_url), url.scheme == "https", url.host == "github.com",
          let notes = release.body, let row = notesSHA256(notes, asset: asset.name) else { return nil }
    let hash = digest.dropFirst("sha256:".count).lowercased()
    guard hash.count == 64, hash == row else { return nil }
    return (asset.name, url, asset.size, hash)
}

/// How the app asks GitHub: the answer's status and body, or an error
public typealias UpdateFetch = @Sendable (URL) async throws -> (status: Int, data: Data)

/// The release's build, if it's strictly newer than the game's commit. Only that: the same commit, behind, diverged,
/// no commit (a build without git), one GitHub doesn't know (a local build), no network, the rate limit or any
/// unexpected answer is nil, and nothing is said about it.
public func checkForUpdate(installed: String?, fetch: UpdateFetch) async -> UpdateOffer? {
    if case .newer(let offer) = await checkRelease(installed: installed, fetch: fetch) { return offer }
    return nil
}

public enum UpdateCheck: Equatable {
    case newer(UpdateOffer)
    /// GitHub answered: this game has nothing to update to
    case notNewer
    /// No answer that says either: the network, the rate limit, a local build's commit, CI at work
    case noAnswer
}

/// The same with the difference that matters once an offer has been made: at Update, "not newer" ends it, and "no
/// answer" goes on with the offer so that the download's own checks say what's wrong.
public func checkRelease(installed: String?, fetch: UpdateFetch) async -> UpdateCheck {
    guard let installed, installed.count >= 7, installed.allSatisfy(\.isHexDigit) else { return .notNewer }
    let api = "https://api.github.com/repos/\(UpdateNames.repository)"
    func get(_ path: String) async -> Data? {
        guard let url = URL(string: api + path), let (status, data) = try? await fetch(url), status == 200 else { return nil }
        return data
    }
    guard let releaseData = await get("/releases/tags/\(UpdateNames.tag)"),
          let asset = releaseAsset(releaseData),
          let refData = await get("/git/ref/tags/\(UpdateNames.tag)"),
          var ref = try? JSONDecoder().decode(RefJSON.self, from: refData) else { return .noAnswer }
    // annotated, as CI makes it: the tag object names the commit
    if ref.object.type == "tag" {
        guard let tagData = await get("/git/tags/\(ref.object.sha)"),
              let tag = try? JSONDecoder().decode(RefJSON.self, from: tagData) else { return .noAnswer }
        ref = tag
    }
    guard ref.object.type == "commit", isFullCommit(ref.object.sha) else { return .noAnswer }
    let commit = ref.object.sha
    if commit.hasPrefix(installed.lowercased()) { return .notNewer }
    // The second page of one commit each leaves the commits and the changed files out: the answer is a few kB
    guard let compareData = await get("/compare/\(installed)...\(commit)?per_page=1&page=2"),
          let comparison = try? JSONDecoder().decode(CompareJSON.self, from: compareData) else { return .noAnswer }
    guard comparison.status == "ahead" else { return .notNewer }
    return .newer(UpdateOffer(commit: commit, asset: asset.asset, url: asset.url, size: asset.size, sha256: asset.sha256,
                              installed: installed))
}

/// Nothing of it stays on the internal disk, and a stale answer can't be served
private func updateSessionConfiguration(requestTimeout: TimeInterval, total: TimeInterval) -> URLSessionConfiguration {
    let config = URLSessionConfiguration.ephemeral
    config.requestCachePolicy = .reloadIgnoringLocalCacheData
    config.urlCache = nil
    config.timeoutIntervalForRequest = requestTimeout
    config.timeoutIntervalForResource = total
    config.waitsForConnectivity = false
    config.httpAdditionalHeaders = ["User-Agent": "mohlauncher"]
    return config
}

/// GitHub's answers, for checkForUpdate
public func githubFetch(_ url: URL) async throws -> (status: Int, data: Data) {
    var request = URLRequest(url: url)
    request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
    request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
    let session = URLSession(configuration: updateSessionConfiguration(requestTimeout: 20, total: 40))
    defer { session.finishTasksAndInvalidate() }
    let (data, response) = try await session.data(for: request)
    return ((response as? HTTPURLResponse)?.statusCode ?? 0, data)
}

/// The installed game's commit: nil if there's no game, or its file can't be read (evicted by iCloud, no network)
public func installedCommit(folder: URL) -> String? {
    let exe = folder.appendingPathComponent("\(UpdateNames.game)/Contents/MacOS/openmohaa")
    return (try? Data(contentsOf: exe, options: .mappedIfSafe)).flatMap(buildCommit)
}

// MARK: Stopping

/// What the window and an update's thread tell each other
public final class UpdateFlags: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelled = false
    private var installing = false
    private var onStop: (() -> Void)?

    public init() {}

    /// Stops the update (Cancel, or the window closed), unless its files are moving already: then false
    @discardableResult
    public func stop() -> Bool {
        lock.lock()
        cancelled = true
        let moving = installing
        let handler = onStop
        lock.unlock()
        handler?()
        return !moving
    }

    public var stopped: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelled
    }

    /// The files may move now, unless the update was stopped
    public func beginInstall() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if cancelled { return false }
        installing = true
        return true
    }

    func whenStopped(_ handler: (() -> Void)?) {
        lock.lock()
        onStop = handler
        let already = cancelled
        lock.unlock()
        if already { handler?() }
    }
}

// MARK: The download

private final class DownloadDelegate: NSObject, URLSessionDataDelegate, @unchecked Sendable {
    let file: FileHandle
    let size: Int64
    let progress: @Sendable (Int64) -> Void
    var done: Int64 = 0
    var failure: UpdateError?
    var finish: ((UpdateError?) -> Void)?

    init(file: FileHandle, size: Int64, progress: @escaping @Sendable (Int64) -> Void) {
        (self.file, self.size, self.progress) = (file, size, progress)
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                    completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
        // missing, or still uploading, while CI replaces it
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        let expected = response.expectedContentLength
        if status != 200 || (expected >= 0 && expected != size) {
            failure = .tryLater
            completionHandler(.cancel)
        } else {
            completionHandler(.allow)
        }
    }

    func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
        guard failure == nil else { return }
        done += Int64(data.count)
        if done > size {
            failure = .tryLater
            dataTask.cancel()
            return
        }
        do {
            try file.write(contentsOf: data)
        } catch {
            failure = updateError(from: error)
            dataTask.cancel()
            return
        }
        progress(done)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        if failure == nil, error != nil { failure = .network }
        finish?(failure)
        finish = nil
    }
}

/// The file's SHA-256, from the disk: what will be unpacked is what is checked
public func fileSHA256(_ url: URL) throws -> String {
    let handle = try FileHandle(forReadingFrom: url)
    defer { try? handle.close() }
    var hasher = SHA256()
    while let chunk = try handle.read(upToCount: 1 << 20), !chunk.isEmpty {
        hasher.update(data: chunk)
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
}

/// The download into `to`. Its timeouts: 60 s without data, an hour in all. A wrong size or hash is tryLater (CI at
/// work), and `check` runs between the download and the hash (a test's corrupted file).
public func downloadUpdate(_ offer: UpdateOffer, to: URL, flags: UpdateFlags,
                           configuration: URLSessionConfiguration? = nil,
                           check: ((URL) -> Void)? = nil,
                           progress: @escaping @Sendable (Int64) -> Void) async throws {
    guard FileManager.default.createFile(atPath: to.path, contents: nil) else { throw UpdateError.unwritable }
    let file = try FileHandle(forWritingTo: to)
    let delegate = DownloadDelegate(file: file, size: offer.size, progress: progress)
    let queue = OperationQueue()
    queue.maxConcurrentOperationCount = 1
    let session = URLSession(configuration: configuration ?? updateSessionConfiguration(requestTimeout: 60, total: 3600),
                             delegate: delegate, delegateQueue: queue)
    defer { session.invalidateAndCancel() }
    let task = session.dataTask(with: offer.url)
    let failure: UpdateError? = await withCheckedContinuation { continuation in
        delegate.finish = { continuation.resume(returning: $0) }
        flags.whenStopped { task.cancel() }
        task.resume()
    }
    flags.whenStopped(nil)
    let synced = fsync(file.fileDescriptor) == 0
    try? file.close()
    if flags.stopped { throw UpdateError.cancelled }
    if let failure { throw failure }
    if !synced { throw updateError(errno: errno) }
    check?(to)
    guard let attributes = try? FileManager.default.attributesOfItem(atPath: to.path),
          (attributes[.size] as? NSNumber)?.int64Value == offer.size,
          (try? fileSHA256(to)) == offer.sha256 else { throw UpdateError.tryLater }
}

// MARK: Unpacking and checking

/// The zip's paths: all inside its top folder, no way out of it, nothing that lands on the player's data
public func zipPathsAreSafe(_ paths: [String], top: String) -> Bool {
    guard !paths.isEmpty else { return false }
    for path in paths {
        if path.hasPrefix("/") || path.contains("\\") || path.unicodeScalars.contains(where: { $0.value < 32 }) { return false }
        var parts = path.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        if parts.last == "" { parts.removeLast() } // a folder's entry
        guard parts.first == top else { return false }
        if parts.contains(where: { $0.isEmpty || $0 == "." || $0 == ".." || $0.hasPrefix("._") }) { return false }
        if parts.count > 1, UpdateNames.reserved.contains(parts[1].lowercased()) { return false }
    }
    return true
}

/// A program's exit status and output; it's killed after `timeout` seconds
func runTool(_ path: String, _ arguments: [String], timeout: TimeInterval = 300) -> (status: Int32, output: String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: path)
    process.arguments = arguments
    let pipe = Pipe()
    process.standardOutput = pipe
    process.standardError = pipe
    do { try process.run() } catch { return (-1, "\(error)") }
    let killer = DispatchWorkItem { if process.isRunning { process.terminate() } }
    DispatchQueue.global().asyncAfter(deadline: .now() + timeout, execute: killer)
    let data = pipe.fileHandleForReading.readDataToEndOfFile()
    process.waitUntilExit()
    killer.cancel()
    return (process.terminationStatus, String(decoding: data, as: UTF8.self))
}

/// Not --strict: iCloud's Finder attributes on a synced folder fail it, for a good bundle too (CLAUDE.md)
public func codesignVerifies(_ app: URL) -> Bool {
    runTool("/usr/bin/codesign", ["--verify", "--deep", app.path], timeout: 120).status == 0
}

struct BundleInfo {
    var identifier: String?
    var executable: String?
    var source: String?

    init(app: URL) {
        let url = app.appendingPathComponent("Contents/Info.plist")
        let plist = (try? Data(contentsOf: url)).flatMap {
            try? PropertyListSerialization.propertyList(from: $0, format: nil) as? [String: Any]
        }
        identifier = plist?["CFBundleIdentifier"] as? String
        executable = plist?["CFBundleExecutable"] as? String
        source = plist?[UpdateNames.sourceKey] as? String
    }
}

/// A launcher that comes from the same source as the installed one: both say so. A launcher from before this stamp,
/// or built from changed files, has none.
public func sameLauncherSource(_ a: URL, _ b: URL) -> Bool {
    guard let one = BundleInfo(app: a).source, !one.isEmpty else { return false }
    return one == BundleInfo(app: b).source
}

private func isDirectory(_ url: URL) -> Bool {
    var st = stat()
    return lstat(url.path, &st) == 0 && st.st_mode & S_IFMT == S_IFDIR
}

private func exists(_ url: URL) -> Bool {
    var st = stat()
    return lstat(url.path, &st) == 0
}

private func walk(_ root: URL, _ visit: (URL, stat) throws -> Void) throws {
    var st = stat()
    guard lstat(root.path, &st) == 0 else { throw posixError() }
    try visit(root, st)
    guard st.st_mode & S_IFMT == S_IFDIR else { return }
    for name in try FileManager.default.contentsOfDirectory(atPath: root.path) {
        try walk(root.appendingPathComponent(name), visit)
    }
}

/// What's quarantined at these paths (itself and everything in it). A path that isn't there has nothing.
public func quarantinedPaths(_ roots: [URL]) throws -> [URL] {
    var found: [URL] = []
    for root in roots where exists(root) {
        try walk(root) { url, _ in
            if getxattr(url.path, UpdateNames.quarantine, nil, 0, 0, XATTR_NOFOLLOW) >= 0 { found.append(url) }
        }
    }
    return found
}

/// What the player's install.command does, for these paths only: the quarantine removed, then a look that none is left
public func clearQuarantine(_ roots: [URL]) throws {
    for root in roots where exists(root) {
        try walk(root) { url, _ in
            if removexattr(url.path, UpdateNames.quarantine, XATTR_NOFOLLOW) != 0, errno != ENOATTR { throw posixError() }
        }
    }
    if try !quarantinedPaths(roots).isEmpty { throw UpdateError.failed }
}

/// An unpacked and checked download: ready to move
public struct StagedUpdate: Equatable {
    /// Holds the apps and the small files, under the work folder
    public var stage: URL
    /// The small files: their paths below `stage`
    public var files: [String]
    /// False when the launcher in the download comes from the same source as the installed one
    public var replaceLauncher: Bool
    public var commit: String

    public init(stage: URL, files: [String], replaceLauncher: Bool, commit: String) {
        (self.stage, self.files, self.replaceLauncher, self.commit) = (stage, files, replaceLauncher, commit)
    }
}

/// The zip unpacked into the work folder and everything about it checked. The game folder is untouched up to here.
public func unpackUpdate(zip: URL, asset: String, commit: String, folder: URL,
                         verify: (URL) -> Bool = codesignVerifies) throws -> StagedUpdate {
    guard asset.hasSuffix(".zip") else { throw UpdateError.tryLater }
    let top = String(asset.dropLast(4))
    let listing = runTool("/usr/bin/zipinfo", ["-1", zip.path], timeout: 60)
    guard listing.status == 0 else { throw UpdateError.tryLater }
    let paths = listing.output.split(separator: "\n").map(String.init)
    guard zipPathsAreSafe(paths, top: top) else { throw UpdateError.tryLater }

    let work = zip.deletingLastPathComponent()
    let unpacked = work.appendingPathComponent("new")
    try? FileManager.default.removeItem(at: unpacked)
    let ditto = runTool("/usr/bin/ditto", ["-x", "-k", "--noqtn", zip.path, unpacked.path])
    guard ditto.status == 0 else {
        if ditto.output.contains("No space left") { throw UpdateError.diskFull }
        if ditto.output.contains("Permission denied") || ditto.output.contains("not permitted") { throw UpdateError.unwritable }
        throw UpdateError.tryLater
    }
    // what ditto made, whatever the listing said: one folder, and only folders and files in it. On a volume without
    // extended attributes (exFAT, FAT32) macOS keeps them in ._ files beside each file, and moves them with it.
    func sidecar(_ name: String) -> Bool { name.hasPrefix("._") }
    guard (try? FileManager.default.contentsOfDirectory(atPath: unpacked.path))?.filter({ !sidecar($0) }) == [top] else {
        throw UpdateError.tryLater
    }
    let stage = unpacked.appendingPathComponent(top)
    let game = stage.appendingPathComponent(UpdateNames.game)
    let launcher = stage.appendingPathComponent(UpdateNames.launcher)
    guard isDirectory(game), isDirectory(launcher) else { throw UpdateError.tryLater }
    var files: [String] = []
    let base = stage.path + "/"
    try walk(stage) { url, st in
        switch st.st_mode & S_IFMT {
        case S_IFDIR: break
        case S_IFREG:
            let inner = String(url.path.dropFirst(base.count))
            if !inner.hasPrefix(UpdateNames.game + "/"), !inner.hasPrefix(UpdateNames.launcher + "/"), !sidecar(url.lastPathComponent) {
                files.append(inner)
            }
        default: throw UpdateError.tryLater // a link could point anywhere
        }
    }
    files.sort()
    // none comes out of a download this way (measured), but a quarantined app would stop at Gatekeeper; ditto gives
    // the top folder one in iCloud's folder
    try clearQuarantine([stage])

    let oldGame = BundleInfo(app: folder.appendingPathComponent(UpdateNames.game))
    let oldLauncher = BundleInfo(app: folder.appendingPathComponent(UpdateNames.launcher))
    let newGame = BundleInfo(app: game)
    let newLauncher = BundleInfo(app: launcher)
    guard let id = newGame.identifier, id == oldGame.identifier,
          let lid = newLauncher.identifier, lid == oldLauncher.identifier else { throw UpdateError.failed }
    for (app, info) in [(game, newGame), (launcher, newLauncher)] {
        guard let exe = info.executable, access(app.appendingPathComponent("Contents/MacOS/\(exe)").path, X_OK) == 0,
              verify(app) else { throw UpdateError.failed }
    }
    // the zip is the tag's build, or it's CI at work
    guard let built = installedCommit(folder: stage), commit.hasPrefix(built) else { throw UpdateError.tryLater }
    return StagedUpdate(stage: stage, files: files, replaceLauncher: !sameLauncherSource(launcher, folder.appendingPathComponent(UpdateNames.launcher)),
                        commit: commit)
}

// MARK: The swap, the rollback and recovery

/// RENAME_SWAP only where the volume says it has it. It must not be tried and judged by its result: FAT32 returns 0
/// and overwrites the target, and exFAT and HFS+ say ENOTSUP (macOS 26, the plan's §3.1 #65).
public func volumeSupportsSwap(_ path: String) -> Bool {
    struct Answer {
        var length: UInt32 = 0
        var capabilities = vol_capabilities_attr_t()
    }
    var list = attrlist()
    list.bitmapcount = u_short(ATTR_BIT_MAP_COUNT)
    list.volattr = attrgroup_t(ATTR_VOL_INFO) | attrgroup_t(ATTR_VOL_CAPABILITIES)
    var answer = Answer()
    guard getattrlist(path, &list, &answer, MemoryLayout<Answer>.size, 0) == 0 else { return false }
    let bit = UInt32(VOL_CAP_INT_RENAME_SWAP)
    return answer.capabilities.valid.1 & bit != 0 && answer.capabilities.capabilities.1 & bit != 0
}

struct UpdateJournal: Codable, Equatable {
    enum Phase: String, Codable { case started, committed }

    struct App: Codable, Equatable {
        var name: String
        /// What it must be to count as the player's own reinstall
        var identifier: String?
        /// With a swap, the two bundles' inodes tell which is where: they keep them, whatever iCloud does to the rest
        var oldInode: UInt64
        var newInode: UInt64
    }

    struct File: Codable, Equatable {
        var path: String
        var hadOld: Bool
    }

    var commit: String
    var useSwap: Bool
    var phase: Phase
    /// The stage, relative to the work folder
    var stage: String
    var apps: [App]
    var files: [File]
    /// Folders the small files needed, deepest last
    var createdFolders: [String]
}

public enum RecoveryOutcome: Equatable {
    /// There was nothing to do
    case nothing
    /// An update stopped before it moved anything: its files are gone
    case cleaned
    /// An update that had moved its apps is done now
    case finished(commit: String)
    /// An update was undone; if `launcherReplaced`, the launcher that's running isn't the one in the folder any more
    case undone(launcherReplaced: Bool)
    /// The game is running: try again when it has quit
    case gameRunning
    /// The folder can't be read now (macOS's folder access was refused): nothing was touched, nothing is known
    case noAccess
    /// What the folder holds isn't what the journal knows, or the undoing failed: nothing more is touched
    case stuck
}

/// Moves a checked download into the game folder, and puts things right after an interruption. Every step has a name,
/// `failpoint` is called with it just before, and the tests make each one fail.
public final class UpdateInstaller {
    public typealias Failpoint = (String) throws -> Void

    let folder: URL
    let failpoint: Failpoint
    let forceRename: Bool
    let verify: (URL) -> Bool

    public init(folder: URL, failpoint: @escaping Failpoint = { _ in }, forceRename: Bool = false,
                verify: @escaping (URL) -> Bool = codesignVerifies) {
        (self.folder, self.failpoint, self.forceRename, self.verify) = (folder, failpoint, forceRename, verify)
    }

    var work: URL { folder.appendingPathComponent(UpdateNames.workFolder) }
    private var journalFile: URL { work.appendingPathComponent("journal.json") }
    private func stage(_ j: UpdateJournal) -> URL { work.appendingPathComponent(j.stage) }
    private var oldFolder: URL { work.appendingPathComponent("old") }
    private var launcherAside: URL { folder.appendingPathComponent(UpdateNames.launcherAside) }

    /// Where an old app waits, without RENAME_SWAP
    private func aside(_ name: String) -> URL {
        name == UpdateNames.launcher ? launcherAside : oldFolder.appendingPathComponent(name)
    }

    private func inode(_ url: URL) -> UInt64? {
        var st = stat()
        return lstat(url.path, &st) == 0 ? UInt64(st.st_ino) : nil
    }

    private func rawRename(_ from: URL, _ to: URL) throws {
        if rename(from.path, to.path) != 0 { throw posixError() }
    }

    private func syncFolder(_ url: URL) {
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { return }
        if fcntl(fd, F_FULLFSYNC) != 0 { fsync(fd) }
        close(fd)
    }

    /// Atomic and on the disk before anything it speaks of happens
    private func writeJournal(_ journal: UpdateJournal) throws {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        let data = try encoder.encode(journal)
        let temporary = work.appendingPathComponent("journal.tmp")
        guard FileManager.default.createFile(atPath: temporary.path, contents: nil) else { throw UpdateError.unwritable }
        let handle = try FileHandle(forWritingTo: temporary)
        do {
            try handle.write(contentsOf: data)
            if fcntl(handle.fileDescriptor, F_FULLFSYNC) != 0, fsync(handle.fileDescriptor) != 0 { throw posixError() }
            try handle.close()
        } catch {
            try? handle.close()
            throw error
        }
        try rawRename(temporary, journalFile)
        syncFolder(work)
    }

    /// The new files on the disk before a rename can make them the game's
    private func syncTree(_ root: URL) throws {
        try walk(root) { url, st in
            let fd = open(url.path, O_RDONLY)
            guard fd >= 0 else { throw posixError() }
            defer { close(fd) }
            if fcntl(fd, F_FULLFSYNC) != 0, fsync(fd) != 0, st.st_mode & S_IFMT != S_IFDIR { throw posixError() }
        }
    }

    private func apps(_ staged: StagedUpdate) -> [String] {
        staged.replaceLauncher ? [UpdateNames.game, UpdateNames.launcher] : [UpdateNames.game]
    }

    private func makeJournal(_ staged: StagedUpdate) throws -> UpdateJournal {
        let base = work.path + "/"
        guard staged.stage.path.hasPrefix(base) else { throw UpdateError.failed }
        var appEntries: [UpdateJournal.App] = []
        for name in apps(staged) {
            let old = folder.appendingPathComponent(name)
            let new = staged.stage.appendingPathComponent(name)
            guard isDirectory(old), isDirectory(new), let oldInode = inode(old), let newInode = inode(new) else {
                throw UpdateError.failed
            }
            appEntries.append(.init(name: name, identifier: BundleInfo(app: new).identifier, oldInode: oldInode, newInode: newInode))
        }
        var fileEntries: [UpdateJournal.File] = []
        var created: [String] = []
        for path in staged.files {
            let target = folder.appendingPathComponent(path)
            if isDirectory(target) { throw UpdateError.failed } // never in place of a folder of the player's
            fileEntries.append(.init(path: path, hadOld: exists(target)))
            var parents: [String] = []
            var parent = (path as NSString).deletingLastPathComponent
            while !parent.isEmpty {
                let url = folder.appendingPathComponent(parent)
                if exists(url) {
                    if !isDirectory(url) { throw UpdateError.failed }
                } else if !created.contains(parent) {
                    parents.insert(parent, at: 0)
                }
                parent = (parent as NSString).deletingLastPathComponent
            }
            created += parents
        }
        let useSwap = !forceRename && volumeSupportsSwap(folder.path)
        // an earlier update's, with no journal left to want it
        if exists(launcherAside) { try FileManager.default.removeItem(at: launcherAside) }
        return UpdateJournal(commit: staged.commit, useSwap: useSwap, phase: .started,
                             stage: String(staged.stage.path.dropFirst(base.count)),
                             apps: appEntries, files: fileEntries, createdFolders: created)
    }

    // An app's state, from what the folder holds
    private enum AppState { case old, aside, new, unknown }

    private func state(_ app: UpdateJournal.App, _ j: UpdateJournal) -> AppState {
        let target = folder.appendingPathComponent(app.name)
        if j.useSwap {
            switch inode(target) {
            case app.newInode?: return .new
            case app.oldInode?: return .old
            default: return .unknown
            }
        }
        let inStage = exists(stage(j).appendingPathComponent(app.name))
        let inOld = exists(aside(app.name))
        switch (exists(target), inStage, inOld) {
        case (true, true, false): return .old
        case (false, true, true): return .aside
        case (true, false, true): return .new
        default: return .unknown
        }
    }

    /// The new app in place, from any state on the way
    private func moveIn(_ app: UpdateJournal.App, _ j: UpdateJournal) throws {
        let target = folder.appendingPathComponent(app.name)
        let new = stage(j).appendingPathComponent(app.name)
        if j.useSwap {
            guard state(app, j) == .old else { return }
            try failpoint("swap:\(app.name)")
            if renamex_np(target.path, new.path, UInt32(RENAME_SWAP)) != 0 { throw posixError() }
        } else {
            if state(app, j) == .old {
                try FileManager.default.createDirectory(at: oldFolder, withIntermediateDirectories: true)
                try failpoint("aside:\(app.name)")
                try rawRename(target, aside(app.name))
            }
            if state(app, j) == .aside {
                try failpoint("place:\(app.name)")
                try rawRename(new, target)
            }
        }
        syncFolder(folder)
    }

    /// The old app back in place, from any state on the way
    private func moveOut(_ app: UpdateJournal.App, _ j: UpdateJournal) throws {
        let target = folder.appendingPathComponent(app.name)
        let new = stage(j).appendingPathComponent(app.name)
        if j.useSwap {
            guard state(app, j) == .new else { return }
            try failpoint("undo-swap:\(app.name)")
            if renamex_np(target.path, new.path, UInt32(RENAME_SWAP)) != 0 { throw posixError() }
        } else {
            if state(app, j) == .new {
                try failpoint("undo-place:\(app.name)")
                try rawRename(target, new)
            }
            if state(app, j) == .aside {
                try failpoint("undo-aside:\(app.name)")
                try rawRename(aside(app.name), target)
            }
        }
        syncFolder(folder)
    }

    private func moveIn(_ file: UpdateJournal.File, _ j: UpdateJournal) throws {
        let target = folder.appendingPathComponent(file.path)
        let new = stage(j).appendingPathComponent(file.path)
        let old = oldFolder.appendingPathComponent(file.path)
        if file.hadOld, !exists(old), exists(target), exists(new) {
            try FileManager.default.createDirectory(at: old.deletingLastPathComponent(), withIntermediateDirectories: true)
            try failpoint("file-aside:\(file.path)")
            try rawRename(target, old)
        }
        if exists(new), !exists(target) {
            try failpoint("file-place:\(file.path)")
            try rawRename(new, target)
        }
    }

    private func moveOut(_ file: UpdateJournal.File, _ j: UpdateJournal) throws {
        let target = folder.appendingPathComponent(file.path)
        let new = stage(j).appendingPathComponent(file.path)
        let old = oldFolder.appendingPathComponent(file.path)
        // placed: the new one goes back. With no old one to come back, that's so whenever it isn't in the stage.
        if exists(target), !exists(new), !file.hadOld || exists(old) {
            try failpoint("undo-file-place:\(file.path)")
            try rawRename(target, new)
        }
        if file.hadOld, exists(old), !exists(target) {
            try failpoint("undo-file-aside:\(file.path)")
            try rawRename(old, target)
        }
    }

    private func forward(_ j: UpdateJournal) throws {
        var journal = j
        for app in j.apps {
            try moveIn(app, j)
            guard state(app, j) == .new else { throw UpdateError.failed }
        }
        for name in j.createdFolders {
            try failpoint("folder:\(name)")
            try FileManager.default.createDirectory(at: folder.appendingPathComponent(name), withIntermediateDirectories: true)
        }
        for file in j.files { try moveIn(file, j) }
        syncFolder(folder)
        try failpoint("commit")
        journal.phase = .committed
        try writeJournal(journal)
    }

    /// Everything back, last first. Its steps can be run again: it starts from what the folder holds.
    private func rollback(_ j: UpdateJournal) throws {
        for file in j.files.reversed() { try moveOut(file, j) }
        for name in j.createdFolders.reversed() { _ = rmdir(folder.appendingPathComponent(name).path) }
        for app in j.apps.reversed() {
            try moveOut(app, j)
            guard state(app, j) == .old else { throw UpdateError.interrupted }
        }
        // Everything's back, and no journal means nothing to undo. Not a new journal: on a full disk that write fails,
        // and the folder that's already fine would be reported as broken.
        try failpoint("undone")
        if unlink(journalFile.path) != 0, errno != ENOENT { throw posixError() }
        syncFolder(work)
    }

    /// After the commit or the rollback nothing here matters any more, so a failure to remove it doesn't either
    private func removeWork() throws {
        try failpoint("cleanup")
        let fm = FileManager.default
        if exists(launcherAside) { try fm.removeItem(at: launcherAside) }
        // the journal last: without it, what's left is known to be nothing anyone needs (recover)
        for name in try fm.contentsOfDirectory(atPath: work.path) where name != journalFile.lastPathComponent {
            try fm.removeItem(at: work.appendingPathComponent(name))
        }
        try fm.removeItem(at: work)
    }

    private func removeWorkQuietly() throws {
        do {
            try removeWork()
        } catch let crash as UpdateCrash {
            throw crash
        } catch {}
    }

    /// The apps, then the small files. An error leaves the folder as it was: the game goes back if the launcher can't
    /// follow it, so the pair always matches. .interrupted if the undoing failed too: the journal stays for recover.
    /// The work folder stays when the launcher was replaced: the old bundle in it is running, and the next start
    /// removes it (`finishInstall`).
    public func install(_ staged: StagedUpdate, gameRunning: () -> Bool = { false }) throws {
        let journal = try makeJournal(staged)
        try syncTree(staged.stage)
        // again, as close to the first move as it can be: that sync can take a while on a slow disk
        if gameRunning() { throw UpdateError.gameRunning }
        try failpoint("journal")
        try writeJournal(journal)
        do {
            try forward(journal)
        } catch let crash as UpdateCrash {
            throw crash
        } catch {
            let reason = updateError(from: error)
            do {
                try rollback(journal)
            } catch let crash as UpdateCrash {
                throw crash
            } catch {
                throw UpdateError.interrupted
            }
            try removeWorkQuietly()
            throw reason
        }
    }

    /// The work folder removed when the update didn't replace the running launcher's bundle
    public func finishInstall(replacedLauncher: Bool) throws {
        if !replacedLauncher { try removeWorkQuietly() }
    }

    /// Anything but folders where the old files wait, or the launcher's old bundle beside the game
    private var hasOldFiles: Bool {
        if exists(launcherAside) { return true }
        guard exists(oldFolder) else { return false }
        var found = false
        try? walk(oldFolder) { _, st in
            if st.st_mode & S_IFMT != S_IFDIR { found = true }
        }
        return found
    }

    private func readJournal() throws -> UpdateJournal? {
        let data: Data
        do {
            data = try Data(contentsOf: journalFile)
        } catch {
            let code = updateErrorCode(error)
            if code == ENOENT { return nil }
            throw error
        }
        return try JSONDecoder().decode(UpdateJournal.self, from: data)
    }

    private func updateErrorCode(_ error: Error) -> Int32 {
        var current: NSError? = error as NSError
        while let e = current {
            if e.domain == NSPOSIXErrorDomain { return Int32(e.code) }
            if e.domain == NSCocoaErrorDomain, e.code == NSFileReadNoSuchFileError { return ENOENT }
            current = e.userInfo[NSUnderlyingErrorKey] as? NSError
        }
        return 0
    }

    /// At every start: what an interrupted update left, finished or undone from the journal, then the work folder
    /// removed. Finished when all its apps had moved and they verify, since only small files were left; undone
    /// otherwise, so that the pair matches. Throws only a test's UpdateCrash.
    public func recover(gameRunning: () -> Bool = { false }) throws -> RecoveryOutcome {
        var st = stat()
        if lstat(work.path, &st) != 0 {
            if errno == ENOENT { return .nothing }
            return .noAccess
        }
        let journal: UpdateJournal?
        do {
            journal = try readJournal()
        } catch is DecodingError {
            return .stuck // written atomically: this isn't ours to judge
        } catch {
            return .noAccess
        }
        guard let journal else {
            // None is written before the first move and removed after the last undoing. Old files with no journal
            // would mean it never reached the disk though the moves did (a volume that doesn't keep the order): those
            // files aren't for removing.
            if hasOldFiles { return .stuck }
            try removeWorkQuietly()
            return .cleaned
        }
        if journal.phase == .committed {
            try removeWorkQuietly()
            return .finished(commit: journal.commit)
        }
        if gameRunning() { return .gameRunning }
        let states = journal.apps.map { state($0, journal) }
        if states.contains(.unknown) {
            // what the update's message tells a player to do: fresh apps in place, whole and the right ones. Then
            // that's the install, and what's here is an old story. Anything else isn't this code's to guess at.
            let reinstalled = journal.apps.allSatisfy {
                let app = folder.appendingPathComponent($0.name)
                return isDirectory(app) && BundleInfo(app: app).identifier == $0.identifier && verify(app)
            }
            guard reinstalled else { return .stuck }
            try removeWorkQuietly()
            return .cleaned
        }
        let allNew = states.allSatisfy { $0 == .new }
        let verified = allNew && journal.apps.allSatisfy { verify(folder.appendingPathComponent($0.name)) }
        // the signatures can take a while, and the game may have started meanwhile
        if gameRunning() { return .gameRunning }
        if verified {
            do {
                try forward(journal)
                try removeWorkQuietly()
                return .finished(commit: journal.commit)
            } catch let crash as UpdateCrash {
                throw crash
            } catch {} // on to the undoing
        }
        // moved at all: whichever launcher is running, the folder's isn't known to be that one afterwards
        let launcherWasNew = journal.apps.contains { $0.name == UpdateNames.launcher && state($0, journal) != .old }
        do {
            try rollback(journal)
        } catch let crash as UpdateCrash {
            throw crash
        } catch {
            return .stuck
        }
        try removeWorkQuietly()
        return .undone(launcherReplaced: launcherWasNew)
    }
}

// MARK: The whole update

/// How far an update has got
public enum UpdateStage: Sendable {
    /// with the bytes so far
    case download(Int64)
    case unpack
    /// the files are moving: it can't be stopped any more
    case install
}

/// Only one launcher works on a folder at a time; the system lets go when the process ends, however it ends
public final class UpdateLock {
    public enum Result {
        case locked(UpdateLock)
        /// Another launcher has it
        case busy
        /// The folder can't be opened: macOS's folder access was refused
        case noAccess
    }

    private var fd: Int32

    private init(fd: Int32) {
        self.fd = fd
    }

    /// A volume without locks gets none, and works as if it had it
    public static func take(folder: URL) -> Result {
        let fd = open(folder.path, O_RDONLY | O_CLOEXEC)
        guard fd >= 0 else { return .noAccess }
        if flock(fd, LOCK_EX | LOCK_NB) != 0, errno == EWOULDBLOCK {
            close(fd)
            return .busy
        }
        return .locked(UpdateLock(fd: fd))
    }

    public func release() {
        if fd >= 0 { close(fd) }
        fd = -1
    }

    deinit { release() }
}

/// A game run from the folder, however it was started (not only what AppKit knows of)
public func gameIsRunning(folder: URL) -> Bool {
    guard let resolved = realpath(folder.path, nil) else { return false }
    defer { free(resolved) }
    let real = String(cString: resolved).lowercased()
    let suffix = "/contents/macos/openmohaa"
    let count = proc_listallpids(nil, 0)
    guard count > 0 else { return false }
    var pids = [pid_t](repeating: 0, count: Int(count) + 32)
    let got = pids.withUnsafeMutableBytes { proc_listallpids($0.baseAddress, Int32($0.count)) }
    guard got > 0 else { return false }
    var buffer = [CChar](repeating: 0, count: 4096)
    for pid in pids.prefix(Int(got)) where pid > 0 {
        guard proc_pidpath(pid, &buffer, UInt32(buffer.count)) > 0 else { continue }
        let path = String(cString: buffer).lowercased()
        if path.hasSuffix(suffix), path.hasPrefix(real + "/") { return true }
    }
    return false
}

/// What a test build's hooks can do to an update; a release has none (bundle.sh, release.sh)
public struct UpdateHooks {
    public var afterDownload: ((URL) -> Void)?
    public var beforeInstall: ((inout StagedUpdate) -> Void)?
    public var downloadURL: URL?
    public var failpoint: UpdateInstaller.Failpoint = { _ in }
    public init() {}
}

/// The update: downloaded into the game folder's work folder, unpacked and checked there, then moved in. True if it
/// replaced the launcher. The caller holds the UpdateLock.
public func runUpdate(_ found: UpdateOffer, folder: URL, flags: UpdateFlags, hooks: UpdateHooks = UpdateHooks(),
                      configuration: URLSessionConfiguration? = nil,
                      verify: @escaping (URL) -> Bool = codesignVerifies,
                      report: @escaping @Sendable (UpdateStage) -> Void) async throws -> Bool {
    let fm = FileManager.default
    var offer = found
    if let url = hooks.downloadURL { offer.url = url }
    if gameIsRunning(folder: folder) { throw UpdateError.gameRunning }
    let installer = UpdateInstaller(folder: folder, failpoint: hooks.failpoint, verify: verify)
    let work = installer.work
    switch try installer.recover(gameRunning: { gameIsRunning(folder: folder) }) {
    case .nothing, .cleaned, .finished, .undone: break
    case .gameRunning: throw UpdateError.gameRunning
    case .noAccess: throw UpdateError.unwritable
    case .stuck: throw UpdateError.interrupted
    }
    do {
        if exists(work) { try fm.removeItem(at: work) }
        try fm.createDirectory(at: work, withIntermediateDirectories: false)
        // the zip, what it unpacks to and the old apps' room to spare
        if let free = try? work.resourceValues(forKeys: [.volumeAvailableCapacityKey]).volumeAvailableCapacity,
           Int64(free) < offer.size * 5 { throw UpdateError.diskFull }
        let zip = work.appendingPathComponent(offer.asset)
        try await downloadUpdate(offer, to: zip, flags: flags, configuration: configuration, check: hooks.afterDownload) {
            report(.download($0))
        }
        if flags.stopped { throw UpdateError.cancelled }
        report(.unpack)
        var staged = try unpackUpdate(zip: zip, asset: offer.asset, commit: offer.commit, folder: folder, verify: verify)
        hooks.beforeInstall?(&staged)
        if gameIsRunning(folder: folder) { throw UpdateError.gameRunning }
        guard flags.beginInstall() else { throw UpdateError.cancelled }
        report(.install)
        try installer.install(staged, gameRunning: { gameIsRunning(folder: folder) })
        try installer.finishInstall(replacedLauncher: staged.replaceLauncher)
        return staged.replaceLauncher
    } catch UpdateError.interrupted {
        throw UpdateError.interrupted // the journal stays
    } catch let crash as UpdateCrash {
        throw crash
    } catch {
        try? fm.removeItem(at: work)
        throw updateError(from: error)
    }
}

// MARK: Where it may run

public enum Translocation: Equatable {
    case no
    /// macOS runs it from a temporary read-only copy (Open Anyway); the real one's address, if it's known
    case yes(original: URL?)
}

private let securityFramework = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW)

public func translocation(of bundle: URL) -> Translocation {
    typealias IsTranslocated = @convention(c) (CFURL, UnsafeMutablePointer<Bool>, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> Bool
    typealias Original = @convention(c) (CFURL, UnsafeMutablePointer<Unmanaged<CFError>?>?) -> Unmanaged<CFURL>?
    if let symbol = dlsym(securityFramework, "SecTranslocateIsTranslocatedURL") {
        var yes = false
        if unsafeBitCast(symbol, to: IsTranslocated.self)(bundle as CFURL, &yes, nil) {
            guard yes else { return .no }
            if let symbol2 = dlsym(securityFramework, "SecTranslocateCreateOriginalPathForURL") {
                let original = unsafeBitCast(symbol2, to: Original.self)(bundle as CFURL, nil)
                return .yes(original: original?.takeRetainedValue() as URL?)
            }
            return .yes(original: nil)
        }
    }
    // the call isn't there, or failed: what the system's own mount looks like (`mount` shows nullfs, read-only)
    var fs = statfs()
    guard statfs(bundle.path, &fs) == 0 else { return bundle.path.contains("/AppTranslocation/") ? .yes(original: nil) : .no }
    let name = withUnsafePointer(to: &fs.f_fstypename) {
        $0.withMemoryRebound(to: CChar.self, capacity: Int(MFSTYPENAMELEN)) { String(cString: $0) }
    }
    return bundle.path.contains("/AppTranslocation/") || (name == "nullfs" && fs.f_flags & UInt32(MNT_RDONLY) != 0)
        ? .yes(original: nil) : .no
}

/// The download's items in a folder that holds it, as install.command clears them: not the game's data
public func installedItems(folder: URL) -> [URL] {
    [UpdateNames.launcher, UpdateNames.game, "INSTALL.txt", "COPYING.txt", "licenses"].map { folder.appendingPathComponent($0) }
}

/// The folder an update works on: the launcher and the game side by side in a real folder of ours. Only then can the
/// pair be kept matching, so otherwise (a launcher renamed or kept elsewhere) nothing is offered.
public func updateFolder(launcherBundle: URL) -> URL? {
    guard launcherBundle.lastPathComponent == UpdateNames.launcher else { return nil }
    let folder = launcherBundle.deletingLastPathComponent()
    guard isDirectory(launcherBundle), isDirectory(folder.appendingPathComponent(UpdateNames.game)) else { return nil }
    return folder
}
