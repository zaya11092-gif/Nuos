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
            pbx.ReadFromFile(report.summary.outputPath + "/Unity-iPhone.xcodeproj/project.pbxproj");
            var mainTarget = pbx.GetUnityFrameworkTargetGuid();
            foreach (var fw in k_Frameworks)
                pbx.AddFrameworkToProject(mainTarget, fw + ".framework", false);
            pbx.WriteToFile(report.summary.outputPath +
                            "/Unity-iPhone.xcodeproj/project.pbxproj");
            Debug.Log("[PVRFrameworks] linked: " + string.Join(", ", k_Frameworks));
#endif
        }
    }
}
