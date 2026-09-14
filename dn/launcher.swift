//
// launcher.swift: open documents in another mode of Dawn
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

import AppKit

// The bundles share this executable; their plists supply the launch mode.
// Dawn hands the documents to its running instance, or becomes that instance.
final class Launcher: NSObject, NSApplicationDelegate {
	private var receivedDocuments = false
	private var submittedEmptyLaunch = false
	private let mainApp = Bundle.main.bundleURL.deletingLastPathComponent()
		.appendingPathComponent("Dawn.app", isDirectory: true)

	func application(_ application: NSApplication, open urls: [URL]) {
		receivedDocuments = true
		launch(urls)
	}

	func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool {
		return !receivedDocuments && !submittedEmptyLaunch
	}

	func applicationOpenUntitledFile(_ sender: NSApplication) -> Bool {
		if !receivedDocuments && !submittedEmptyLaunch {
			submittedEmptyLaunch = true
			launch([])
		}
		return true
	}

	func applicationShouldHandleReopen(_ sender: NSApplication,
									  hasVisibleWindows flag: Bool) -> Bool {
		launch([])
		// We handled it; do not also request an untitled document.
		return false
	}

	private func launch(_ urls: [URL]) {
		if let failure = run(urls) {
			let alert = NSAlert()
			alert.messageText = "Could not open Dawn"
			let documents = urls.map { $0.path }.joined(separator: "\n")
			alert.informativeText = "\(mainApp.path)\n\(documents)\n\n\(failure)"
			NSApp.activate(ignoringOtherApps: true)
			alert.runModal()
		}
		// Give any already queued open events a chance to join this request.
		DispatchQueue.main.async { NSApp.terminate(nil) }
	}

	private func run(_ urls: [URL]) -> String? {
		guard let mode = Bundle.main.object(
				forInfoDictionaryKey: "DawnMode") as? String else {
			return "The launcher has no application mode."
		}
		guard let dawn = Bundle(url: mainApp),
			  let executable = dawn.executableURL else {
			return "Dawn is not installed next to this launcher."
		}

		let process = Process()
		process.executableURL = executable
		process.arguments = ["--mode=\(mode)", "--"] + urls.map { $0.path }
		do {
			try process.run()
		} catch {
			return error.localizedDescription
		}

		// Only the active application may pass activation on,
		// and whichever process ends up with the window asks for it.
		if #available(macOS 14.0, *), let id = dawn.bundleIdentifier {
			NSApp.yieldActivation(toApplicationWithBundleIdentifier: id)
		}
		return nil
	}
}

let application = NSApplication.shared
let delegate = Launcher()
application.setActivationPolicy(.accessory)
application.delegate = delegate
application.run()
