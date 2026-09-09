//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

import AppKit

// The two bundles share this executable; their plists supply the launch mode.
final class Launcher: NSObject, NSApplicationDelegate {
	private var pending = 0
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
		pending += 1
		guard let mode = Bundle.main.object(
				forInfoDictionaryKey: "DawnMode") as? String,
			mode == "cropjpeg" || mode == "commander" else {
			finish("The launcher has no valid application mode.", urls)
			return
		}

		let config = NSWorkspace.OpenConfiguration()
		config.activates = true
		config.createsNewApplicationInstance = true
		config.allowsRunningApplicationSubstitution = false
		config.arguments = ["--mode=\(mode)"]
		let completion: (NSRunningApplication?, Error?) -> Void = { app, error in
			let failure = error?.localizedDescription ??
				(app == nil ? "No application was launched." : nil)
			// NSWorkspace completion handlers may run on a concurrent queue.
			DispatchQueue.main.async { self.finish(failure, urls) }
		}
		if urls.isEmpty {
			NSWorkspace.shared.openApplication(at: mainApp,
											   configuration: config,
											   completionHandler: completion)
		} else {
			NSWorkspace.shared.open(urls, withApplicationAt: mainApp,
									configuration: config,
									completionHandler: completion)
		}
	}

	private func finish(_ failure: String?, _ urls: [URL]) {
		if let failure = failure {
			let alert = NSAlert()
			alert.messageText = "Could not open Dawn"
			let documents = urls.map { $0.path }.joined(separator: "\n")
			alert.informativeText = "\(mainApp.path)\n\(documents)\n\n\(failure)"
			NSApp.activate(ignoringOtherApps: true)
			alert.runModal()
		}
		pending -= 1
		// Give any already queued open events a chance to join this request.
		DispatchQueue.main.async {
			if self.pending == 0 { NSApp.terminate(nil) }
		}
	}
}

let application = NSApplication.shared
let delegate = Launcher()
application.setActivationPolicy(.accessory)
application.delegate = delegate
application.run()
