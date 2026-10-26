using System.Security.Cryptography;

namespace Reborn.Update;

/// <summary>Checks update signatures made by tools/signing.py: ECDSA P-256 over SHA-256,
/// r||s (64 bytes, IEEE P1363) in base64; the public key is the point X||Y (64 bytes) in base64.</summary>
public static class Signature
{
    /// <summary>True only when <paramref name="sigB64"/> is a valid signature of <paramref name="data"/>
    /// by <paramref name="pubB64"/>. Any bad input (bad base64, wrong lengths, a point not on the curve) is false.</summary>
    public static bool Verify(byte[] data, string sigB64, string pubB64)
    {
        try
        {
            byte[] sig = Convert.FromBase64String(sigB64);
            byte[] pub = Convert.FromBase64String(pubB64);
            if (sig.Length != 64 || pub.Length != 64) return false;

            using ECDsa ecdsa = ECDsa.Create();
            ecdsa.ImportParameters(new ECParameters
            {
                Curve = ECCurve.NamedCurves.nistP256,
                Q = { X = pub[..32], Y = pub[32..] },
            });
            return ecdsa.VerifyData(data, sig, HashAlgorithmName.SHA256, DSASignatureFormat.IeeeP1363FixedFieldConcatenation);
        }
        catch (Exception)
        {
            return false;
        }
    }
}
