import UIKit
import UniformTypeIdentifiers

/// Choosing a file to import from, and saving a file exported, through the Files
/// app's own pickers. One of these lives as long as the screen that uses it.
final class FileTransfer: NSObject, UIDocumentPickerDelegate {
    private var onPicked: ((URL) -> Void)?
    private var exported: URL?

    /// Asks for a file with one of the extensions (any file can be chosen, since
    /// lists like these come with all sorts of types); it is handed over as a copy.
    func importFile(extensions: [String], from controller: UIViewController, picked: @escaping (URL) -> Void) {
        var types = extensions.compactMap { UTType(filenameExtension: $0) }
        types.append(contentsOf: [.plainText, .xml, .data])
        let picker = UIDocumentPickerViewController(forOpeningContentTypes: types, asCopy: true)
        picker.delegate = self
        onPicked = picked
        controller.present(picker, animated: true)
    }

    /// Writes a file with `write` and asks where to save it. `write` returns false
    /// if it could not make the file.
    func exportFile(named name: String, from controller: UIViewController, write: (String) -> Bool) {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let url = folder.appendingPathComponent(name)
        guard write(url.path) else {
            let alert = UIAlertController(title: "Export", message: "The file could not be written.", preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default))
            controller.present(alert, animated: true)
            return
        }
        exported = url
        let picker = UIDocumentPickerViewController(forExporting: [url], asCopy: true)
        picker.delegate = self
        onPicked = nil
        controller.present(picker, animated: true)
    }

    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        if let exported {
            // Saved where the user chose: the copy made for it can go
            try? FileManager.default.removeItem(at: exported.deletingLastPathComponent())
            self.exported = nil
            UIAccessibility.post(notification: .announcement, argument: "Exported")
            return
        }
        if let url = urls.first { onPicked?(url) }
        onPicked = nil
    }

    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) {
        if let exported { try? FileManager.default.removeItem(at: exported.deletingLastPathComponent()) }
        exported = nil
        onPicked = nil
    }
}
