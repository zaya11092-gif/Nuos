//
//  NuosViewApp.swift
//  NuosView
//
//  SwiftUI entry point. iPhone landscape only (Info.plist orientations).
//

import SwiftUI

@main
struct NuosViewApp: App {
    @StateObject private var sessionModel = SessionModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(sessionModel)
                .persistentSystemOverlays(.hidden)
        }
    }
}
