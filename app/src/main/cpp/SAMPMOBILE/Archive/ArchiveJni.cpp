#include "ArchiveExtractor.h"

#include <jni.h>
#include <chrono>
#include <string>

namespace {

std::string jstringToUtf8(JNIEnv* env, jstring js)
{
	if (!env || !js) return {};
	const char* chars = env->GetStringUTFChars(js, nullptr);
	if (!chars) return {};
	std::string out(chars);
	env->ReleaseStringUTFChars(js, chars);
	return out;
}

} // namespace

extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_sampmobile_launcher_utils_NativeArchiveExtractor_nativeExtractCacheArchive(
	JNIEnv* env,
	jclass /*clazz*/,
	jstring jArchivePath,
	jstring jLocalCacheDir,
	jobject jBridgeCallback,
	jlongArray jOutNumbers)
{
	// jOutNumbers[5]:
	//   [0] = status (1 = ok, 0 = error, 2 = cancelled, 3 = passwordRequired)
	//   [1] = importedFiles
	//   [2] = skippedFiles
	//   [3] = importedBytes
	const std::string archivePath = jstringToUtf8(env, jArchivePath);
	const std::string localCacheDir = jstringToUtf8(env, jLocalCacheDir);

	jmethodID onProgressMid = nullptr;
	if (jBridgeCallback != nullptr) {
		jclass cbCls = env->GetObjectClass(jBridgeCallback);
		if (cbCls != nullptr) {
			onProgressMid = env->GetMethodID(cbCls, "onNativeProgress", "(IILjava/lang/String;)Z");
			env->DeleteLocalRef(cbCls);
		}
	}

	auto lastNotify = std::chrono::steady_clock::now() - std::chrono::seconds(1);

	samparchive::CacheArchiveResult res = samparchive::extractCacheArchive(
		archivePath,
		localCacheDir,
		[&](int imported, int skipped, const std::string& relPath) -> bool {
			if (!jBridgeCallback || !onProgressMid) {
				return true;
			}
			const auto now = std::chrono::steady_clock::now();
			// Always check cancellation quickly; throttle string allocation when relPath is non-empty
			if (!relPath.empty()) {
				const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastNotify).count();
				if (elapsedMs < 80 && imported > 3) {
					return true;
				}
				lastNotify = now;
			}
			jstring jRel = relPath.empty() ? nullptr : env->NewStringUTF(relPath.c_str());
			const jboolean keepGoing = env->CallBooleanMethod(
				jBridgeCallback, onProgressMid,
				static_cast<jint>(imported), static_cast<jint>(skipped), jRel);
			if (jRel != nullptr) {
				env->DeleteLocalRef(jRel);
			}
			if (env->ExceptionCheck()) {
				env->ExceptionClear();
				return false;
			}
			return keepGoing == JNI_TRUE;
		});

	if (jOutNumbers != nullptr && env->GetArrayLength(jOutNumbers) >= 4) {
		jlong nums[4]{};
		if (res.cancelled) {
			nums[0] = 2;
		} else if (res.passwordRequired) {
			nums[0] = 3;
		} else {
			nums[0] = res.ok ? 1 : 0;
		}
		nums[1] = static_cast<jlong>(res.importedFiles);
		nums[2] = static_cast<jlong>(res.skippedFiles);
		nums[3] = static_cast<jlong>(res.importedBytes);
		env->SetLongArrayRegion(jOutNumbers, 0, 4, nums);
	}

	// Return String[] where element [0] is error message ("" if none) and [1..N] are nested .zip paths
	jclass strCls = env->FindClass("java/lang/String");
	if (!strCls) {
		return nullptr;
	}
	const jsize outLen = static_cast<jsize>(1 + res.nestedZipPaths.size());
	jobjectArray arr = env->NewObjectArray(outLen, strCls, env->NewStringUTF(""));
	if (!arr) {
		return nullptr;
	}
	jstring jErr = env->NewStringUTF(res.error.c_str());
	env->SetObjectArrayElement(arr, 0, jErr);
	if (jErr) env->DeleteLocalRef(jErr);

	for (size_t i = 0; i < res.nestedZipPaths.size(); ++i) {
		jstring jZip = env->NewStringUTF(res.nestedZipPaths[i].c_str());
		env->SetObjectArrayElement(arr, static_cast<jsize>(1 + i), jZip);
		if (jZip) env->DeleteLocalRef(jZip);
	}
	return arr;
}
