# Homebrew formula for tenseleyFlow/homebrew-tap. Activates once the GitHub
# repo/release is public; release automation pins the sha256.
class Tally < Formula
  desc "GNU wc reimplementation: byte-identical output, SIMD word counting"
  homepage "https://github.com/tenseleyFlow/tally"
  url "https://github.com/tenseleyFlow/tally/releases/download/v0.1.0/tally-0.1.0.tar.gz"
  sha256 "8ed3e585500bd0091fcb32beae097d84a3d1b304de46e5b51796d1b976c6358f"
  license "GPL-3.0-or-later"

  def install
    system "./configure"
    system "make", "release"
    bin.install "tally"
    bin.install_symlink "tally" => "ty"
    man1.install "doc/tally.1", "doc/ty.1"
  end

  test do
    assert_equal "0 0 0", shell_output("printf '' | #{bin}/tally").strip
    assert_equal "3", shell_output("printf 'a b c' | #{bin}/ty -w").strip
  end
end
