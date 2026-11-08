using UnityEditor;
using UnityEditor.Build;
using UnityEditor.Build.Reporting;
using UnityEngine;

namespace CardboardXR.PVR
{
    /// <summary>
    /// Links the frameworks PVRDecoder.mm needs into the UnityFramework target
    /// after Unity generates the Xcode project.
    /// </summary>
    public class PVRFrameworks : IPostprocessBuildWithReport
    {
        public int callbackOrder => 10;

        static readonly string[] k_Frameworks =
        {
            "VideoToolbox", "CoreMedia", "CoreVideo", "Metal",
        };

        public void OnPostprocessBuild(BuildReport report)
        {
#if UNITY_IOS
            if (report.summary.platform != BuildTarget.iOS) return;
            var pbx = new UnityEditor.iOS.Xcode.PBXProject();
            var projPath = report.summary.outputPath +
                           "/Unity-iPhone.xcodeproj/project.pbxproj";
            pbx.ReadFromFile(projPath);
            var fwTarget = pbx.GetUnityFrameworkTargetGuid();
            foreach (var fw in k_Frameworks)
                pbx.AddFrameworkToProject(fwTarget, fw + ".framework", false);

            // Plugins/iOS sources land in the app target, but DllImport
            // __Internal resolves inside UnityFramework — move ours over.
            var appTarget = pbx.GetUnityMainTargetGuid();
            var guid = pbx.FindFileGuidByProjectPath(
                "Assets/Plugins/iOS/PVRDecoder.mm");
            if (!string.IsNullOrEmpty(guid))
            {
                pbx.RemoveFileFromBuild(appTarget, guid);
                pbx.AddFileToBuild(fwTarget, guid);
                Debug.Log("[PVRFrameworks] PVRDecoder.mm -> UnityFramework");
            }

            pbx.WriteToFile(projPath);
            Debug.Log("[PVRFrameworks] linked: " + string.Join(", ", k_Frameworks));
#endif
        }
    }
}
