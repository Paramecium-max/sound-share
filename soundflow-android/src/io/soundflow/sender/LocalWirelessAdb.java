package io.soundflow.sender;

import android.content.Context;
import android.os.Build;

import io.github.muntashirakon.adb.AdbStream;
import io.github.muntashirakon.adb.AbsAdbConnectionManager;
import io.github.muntashirakon.adb.android.AdbMdns;

import org.bouncycastle.asn1.ASN1Integer;
import org.bouncycastle.asn1.DERBitString;
import org.bouncycastle.asn1.DERNull;
import org.bouncycastle.asn1.DERSequence;
import org.bouncycastle.asn1.pkcs.PKCSObjectIdentifiers;
import org.bouncycastle.asn1.x500.X500Name;
import org.bouncycastle.asn1.x509.AlgorithmIdentifier;
import org.bouncycastle.asn1.x509.SubjectPublicKeyInfo;
import org.bouncycastle.asn1.x509.TBSCertificate;
import org.bouncycastle.asn1.x509.Time;
import org.bouncycastle.asn1.x509.V3TBSCertificateGenerator;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.security.KeyPair;
import java.security.PrivateKey;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.util.Date;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class LocalWirelessAdb extends AbsAdbConnectionManager {
    private static final String HOST = "127.0.0.1";
    private final KeyPair key;
    private final Certificate certificate;
    private AdbStream daemonStream;

    LocalWirelessAdb(Context context) throws Exception {
        key = WirelessAdbClient.loadOrCreateKeyPair(context);
        certificate = certificateFor(key);
        setApi(Build.VERSION.SDK_INT);
        setTimeout(5, TimeUnit.SECONDS);
        setThrowOnUnauthorised(true);
    }

    @Override protected PrivateKey getPrivateKey() { return key.getPrivate(); }
    @Override protected Certificate getCertificate() { return certificate; }
    @Override protected String getDeviceName() { return "SoundFlow"; }

    static int discover(Context context, String service) throws InterruptedException {
        CountDownLatch found = new CountDownLatch(1);
        AtomicInteger port = new AtomicInteger();
        AdbMdns discovery = new AdbMdns(context, service, (localAddress, announcedPort) -> {
            // AdbMdns only reports addresses assigned to this device.
            if (announcedPort > 0 && port.compareAndSet(0, announcedPort)) found.countDown();
        });
        try {
            discovery.start();
            found.await(3, TimeUnit.SECONDS);
            return port.get();
        } finally {
            discovery.stop();
        }
    }

    void pairLocal(int port, String code) throws Exception {
        pair(HOST, port, code);
    }

    void connectLocal(int port) throws Exception {
        if (!connect(HOST, port)) throw new IOException("本机 TLS 调试端口未建立连接");
    }

    void launchDaemon(String command) throws Exception {
        daemonStream = openStream("shell:" + command);
    }

    @Override public void close() throws IOException {
        try {
            if (daemonStream != null) daemonStream.close();
        } finally {
            daemonStream = null;
            disconnect();
        }
    }

    private static Certificate certificateFor(KeyPair key) throws Exception {
        AlgorithmIdentifier shaRsa = new AlgorithmIdentifier(PKCSObjectIdentifiers.sha256WithRSAEncryption, DERNull.INSTANCE);
        X500Name name = new X500Name("CN=SoundFlow local Android debugging");
        long now = System.currentTimeMillis();
        V3TBSCertificateGenerator generator = new V3TBSCertificateGenerator();
        generator.setSerialNumber(new ASN1Integer(1));
        generator.setIssuer(name);
        generator.setSubject(name);
        generator.setStartDate(new Time(new Date(now - TimeUnit.DAYS.toMillis(1))));
        generator.setEndDate(new Time(new Date(now + TimeUnit.DAYS.toMillis(3650))));
        generator.setSignature(shaRsa);
        generator.setSubjectPublicKeyInfo(SubjectPublicKeyInfo.getInstance(key.getPublic().getEncoded()));
        TBSCertificate certificate = generator.generateTBSCertificate();
        Signature signer = Signature.getInstance("SHA256withRSA");
        signer.initSign(key.getPrivate());
        signer.update(certificate.getEncoded());
        byte[] signed = new DERSequence(new org.bouncycastle.asn1.ASN1Encodable[]{
                certificate, shaRsa, new DERBitString(signer.sign())
        }).getEncoded();
        return CertificateFactory.getInstance("X.509").generateCertificate(new ByteArrayInputStream(signed));
    }
}
